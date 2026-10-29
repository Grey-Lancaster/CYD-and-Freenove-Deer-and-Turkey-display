"""Make the woods ambience loop from the first 25 s of a recording.
  python design/make_ambience.py [input.wav]    (default: the only .wav in the project folder)
Writes src/ambience.wav (11.025 kHz mono 16-bit, CYD: the firmware upsamples x4 to 44.1 kHz; anything bigger does not fit the CYD's flash, and everything above 2.3 kHz is cut anyway) and src/ambience.raw (16 kHz mono 16-bit, Freenove).
The loop is exactly LOOP_S long. The 0.75 s that follows it in the source is blended into the start, so the end
runs straight into the beginning with no click.
"""
import glob, os, sys, wave
import numpy as np

LOOP_S, XFADE_S, PEAK = 25.0, 0.75, 0.8
# The crickets in this recording are a narrow pulsing strip around 3.5 kHz and play as a hiss on a small speaker, so
# just that strip is notched out (CUT_DB down, soft edges). Crow, turkey and bird calls are broadband bursts that
# pass through it. The steady background (hum, wind, hiss) is removed first by denoise().
CUT_LO, CUT_HI, CUT_DB, EDGE = 3200, 4000, 35, 200
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def read_mono(path, seconds):
    w = wave.open(path)
    ch, sw, rate = w.getnchannels(), w.getsampwidth(), w.getframerate()
    raw = w.readframes(int(seconds * rate))
    if sw == 3:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3).astype(np.int32)
        x = (b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16))
        x = np.where(x >= 1 << 23, x - (1 << 24), x) / float(1 << 23)
    elif sw == 2:
        x = np.frombuffer(raw, np.int16).astype(np.float64) / 32768.0
    else:
        raise SystemExit("unsupported sample width %d" % sw)
    x = x.reshape(-1, ch).mean(axis=1)
    return x, rate


def resample(x, r0, r1):
    if r0 == r1:
        return x
    n = int(round(len(x) * r1 / r0))
    t = np.arange(n) * (r0 / r1)
    # band-limit first when going down (simple FFT low-pass), then linear interpolation
    if r1 < r0:
        X = np.fft.rfft(x)
        f = np.fft.rfftfreq(len(x), 1.0 / r0)
        X[f > 0.45 * r1] = 0
        x = np.fft.irfft(X, len(x))
    return np.interp(t, np.arange(len(x)), x)


def denoise(x, rate, n=2048, pct=50, over=1.6, floor_db=24, hp=90):
    """Spectral gate: learn the steady background (hum, wind, cricket drone) as a per-frequency level and
    subtract it, so only sounds that stand out of it (crows, gobbles, bird chirps) get through."""
    hop = n // 4
    w = np.hanning(n)
    pad = np.concatenate([np.zeros(n), x, np.zeros(n)])
    starts = range(0, len(pad) - n, hop)
    S = np.array([np.fft.rfft(pad[i:i + n] * w) for i in starts])
    mag = np.abs(S)
    noise = np.percentile(mag, pct, axis=0)
    noise = np.convolve(noise, np.ones(5) / 5, mode="same")               # smooth the profile across frequency
    g = np.clip(1.0 - over * noise / (mag + 1e-12), 10 ** (-floor_db / 20.0), 1.0)
    g = np.maximum.reduce([g, np.roll(g, 1, axis=0) * 0.6, np.roll(g, -1, axis=0) * 0.6])      # let gates linger a frame either side
    box = np.ones(5) / 5                                                                         # smooth the gains over time and frequency: no watery "musical noise"
    g = np.apply_along_axis(np.convolve, 0, g, np.ones(3) / 3, "same")
    g = np.apply_along_axis(np.convolve, 1, g, box, "same")
    f = np.fft.rfftfreq(n, 1.0 / rate)
    g *= np.clip((f - hp * 0.6) / (hp * 0.4), 0, 1)                      # high-pass: nothing below hp/2 .. hp Hz
    out = np.zeros(len(pad))
    for k, i in enumerate(starts):
        out[i:i + n] += np.fft.irfft(S[k] * g[k], n)
    return out[n:n + len(x)] / 2.0                                        # Hann at 75% overlap sums to 2


def band_cut(x, rate):
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1.0 / rate)
    g = np.clip((f - (CUT_LO - EDGE)) / EDGE, 0, 1) * np.clip(((CUT_HI + EDGE) - f) / EDGE, 0, 1)   # 0..1 inside the cut band
    X *= 10 ** (-CUT_DB * g / 20.0)
    return np.fft.irfft(X, len(x))


def loop(x, rate):
    n, f = int(LOOP_S * rate), int(XFADE_S * rate)
    out = x[:n].copy()
    a = np.linspace(0, 1, f)
    out[:f] = x[:f] * a + x[n:n + f] * (1 - a)       # start fades in from what follows the loop's end
    return out


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else glob.glob(os.path.join(ROOT, "*.wav"))[0]
    x, rate = read_mono(src, LOOP_S + XFADE_S)
    x = band_cut(denoise(x, rate), rate)
    if len(x) < (LOOP_S + XFADE_S) * rate:
        raise SystemExit("recording is shorter than %.2f s" % (LOOP_S + XFADE_S))
    for r, name in ((11025, "ambience.wav"), (16000, "ambience.raw")):
        y = loop(resample(x, rate, r), r)
        y = y / max(1e-9, np.abs(y).max()) * PEAK
        pcm = (y * 32767).astype("<i2")
        path = os.path.join(ROOT, "src", name)
        if r == 16000:                                   # the file to audition on the PC (what the Freenove plays)
            with wave.open(os.path.join(ROOT, "design", "ambience_preview.wav"), "wb") as w:
                w.setnchannels(1); w.setsampwidth(2); w.setframerate(r); w.writeframes(pcm.tobytes())
        if name.endswith(".wav"):
            with wave.open(path, "wb") as w:
                w.setnchannels(1); w.setsampwidth(2); w.setframerate(r); w.writeframes(pcm.tobytes())
        else:
            open(path, "wb").write(pcm.tobytes())
        print(name, "%.1f s" % (len(pcm) / r), os.path.getsize(path) // 1024, "KB")


if __name__ == "__main__":
    main()
