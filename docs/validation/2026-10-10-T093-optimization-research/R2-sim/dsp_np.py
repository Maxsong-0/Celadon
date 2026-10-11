import numpy as np
def fft_filter(x, fs, lo=None, hi=None):
    X = np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1/fs); m = np.ones_like(f)
    if lo is not None: m *= (f >= lo)
    if hi is not None: m *= (f <= hi)
    return np.fft.irfft(X*m, len(x))
def up(x, k):
    X = np.fft.rfft(x); n = len(x); Y = np.zeros(n*k//2 + 1, complex); Y[:len(X)] = X
    if n % 2 == 0: Y[len(X)-1] *= 0.5
    return np.fft.irfft(Y, n*k)*k
def down(y, k):
    Y = np.fft.rfft(y); n = len(y)//k; X = Y[:n//2 + 1].copy()
    return np.fft.irfft(X, n)/k
def tp(x, k=8): return np.max(np.abs(up(x, k)))
