#!/usr/bin/env python3
"""T-085 offline codec round trips; never changes plugin parameters or defaults.

Dependencies: numpy, scipy, soundfile, pyloudnorm. Audio stays in --work;
CSV/JSON evidence goes in --out. See the T-085 REPORT.md for definitions.
"""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys

import numpy as np
import pyloudnorm as pyln
import soundfile as sf
from scipy.signal import chirp, firwin, sosfilt, butter
from scipy.fft import rfft, irfft, next_fast_len

CODECS = (('aac', 256, 'm4a'), ('aac', 128, 'm4a'),
          ('libopus', 160, 'opus'), ('libopus', 96, 'opus'),
          ('libmp3lame', 320, 'mp3'), ('libmp3lame', 128, 'mp3'))


def run(command, log=None):
    result = subprocess.run([str(x) for x in command], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, check=True)
    if log is not None:
        Path(log).write_text(result.stdout, encoding='utf-8')
    return result.stdout


def db(value):
    return 20 * math.log10(max(float(value), 1e-15))


def write_csv(path, rows):
    with Path(path).open('w', newline='', encoding='utf-8') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def download_jamendo(args):
    """Repository T-038 manifest, first N IDs per category, full tracks, no redistribution."""
    manifest = Path(__file__).resolve().parents[2] / 'docs/validation/2026-10-01-T038-reference/manifest.tsv'
    with manifest.open() as stream:
        rows = list(csv.DictReader(stream, delimiter='\t'))
    records = []
    args.input.mkdir(parents=True, exist_ok=True)
    download = args.work / 'downloads'
    download.mkdir(parents=True, exist_ok=True)
    for category in ('deep', 'tech', 'melodic'):
        for row in [r for r in rows if r['category'] == category][:args.jamendo_per_class]:
            track = str(int(row['track_id'].split('_')[1]))
            url = f'https://prod-1.storage.jamendo.com/download/track/{track}/mp32/'
            mp3 = download / (track + '.mp3')
            run(['curl', '-sSL', '--fail', '--retry', '2', '--max-time', '120', '-o', mp3, url])
            wav = args.input / f'{category}_{track}.wav'
            run([args.ffmpeg, '-v', 'error', '-nostdin', '-y', '-i', mp3,
                 '-ar', '48000', '-ac', '2', '-c:a', 'pcm_f32le', wav])
            records.append({**row, 'url': url, 'download_sha256': hashlib.sha256(mp3.read_bytes()).hexdigest()})
    write_csv(args.out / 'corpus.csv', records)


def synthetic(folder, seconds=40, rate=48000):
    """Fixed seed; four supplementary stress cases, not substitutes for music."""
    folder.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(85)
    t = np.arange(seconds * rate) / rate
    beat = t % .5
    kick = np.sin(2 * np.pi * (55 * beat + 35 * .025 * (1 - np.exp(-beat / .025)))) * np.exp(-beat * 22)
    noise = rng.standard_normal((len(t), 2))
    hats = sosfilt(butter(4, 7500, 'highpass', fs=rate, output='sos'), noise, axis=0)
    env = np.exp(-(t % .125) * 65)
    bass = np.sin(2 * np.pi * 55 * t)
    cases = {
        'stress_hot_mix': np.tanh(2.2 * (.55 * noise + kick[:, None] + .4 * bass[:, None])),
        'stress_dense_hats': .8 * hats * env[:, None] + .35 * kick[:, None],
        'stress_side_bass': np.column_stack((bass, -bass)) * .8 + .12 * noise,
        'stress_sweep': np.column_stack((chirp(t, 20, seconds, 22000, method='logarithmic'),
                                        chirp(t, 22000, seconds, 20, method='logarithmic'))) * .9,
    }
    for name, audio in cases.items():
        audio *= .97 / np.max(np.abs(audio))
        # Brief fades avoid endpoint discontinuities; preserve the full programme.
        fade = np.minimum(1, np.minimum(t / .02, (seconds - t) / .02))
        audio *= fade[:, None]
        desired = -8.0 if name == 'stress_hot_mix' else -18.0
        loudness = pyln.Meter(rate, filter_class='DeMan').integrated_loudness(audio)
        audio *= min(1.0, 10 ** ((desired - loudness) / 20))
        sf.write(folder / (name + '.wav'), audio, rate, subtype='FLOAT')


def measure(path):
    audio, rate = sf.read(path, always_2d=True, dtype='float64')
    if audio.shape[1] != 2 or not np.all(np.isfinite(audio)) or len(audio) < 3 * rate:
        raise ValueError(f'{path}: requires finite stereo audio >= 3 seconds')
    meter = pyln.Meter(rate, filter_class='DeMan')  # BS.1770 K weighting, 400 ms, 75% overlap, -70 / -10 LU gates.
    loudness = float(meter.integrated_loudness(audio))
    sample_peak = np.max(np.abs(audio))
    result = {'duration_s': len(audio) / rate, 'sample_rate': rate, 'lufs_i': loudness,
              'sample_peak_dbfs': db(sample_peak),
              'sample_clip_count': int(np.count_nonzero(np.abs(audio) > 1)),
              'crest_db': db(sample_peak) - db(np.sqrt(np.mean(audio ** 2)))}
    # Independent linear-phase interpolation: BS.1770 Annex 2 oversampling method.
    # 64 taps per phase, Kaiser beta=12, unity DC; not JUCE's detector nor the
    # illustrative 12-tap/phase ITU coefficients. 4x and 8x reported separately.
    hop = rate // 10
    bins = math.ceil(len(audio) / hop)
    peaks8 = np.zeros(bins)
    for factor in (4, 8):
        kernel = firwin(64 * factor + 1, 1 / factor, window=('kaiser', 12.0))
        peak, count = 0.0, 0
        for start in range(0, len(audio), hop * 100):
            stop = min(start + hop * 100, len(audio))
            lo, hi = max(0, start - 64), min(len(audio), stop + 64)
            # Equivalent to resample_poly's centered FIR, but reuse the input
            # FFT across phases and never allocate a full 8x waveform.
            chunk = audio[lo:hi]
            size = next_fast_len(len(chunk) + 64)
            spectrum = rfft(chunk, n=size, axis=0)
            for phase in range(factor):
                taps = kernel[phase::factor] * factor
                filtered = irfft(spectrum * rfft(taps, n=size)[:, None], n=size, axis=0)
                core = np.abs(filtered[32 + start - lo:32 + stop - lo])
                peak = max(peak, float(np.max(core)))
                if phase != 0:
                    count += int(np.count_nonzero(core > 1))
                if factor == 8:
                    mono = core.max(axis=1)
                    for offset in range(0, stop - start, hop):
                        index = (start + offset) // hop
                        peaks8[index] = max(peaks8[index], mono[offset:offset + hop].max())
        result[f'tp{factor}_dbtp'] = db(peak)
        result[f'intersample_clip{factor}_count'] = count
    # PSR: synchronized trailing 3 s TP minus ungated 3 s K-weighted loudness,
    # every 100 ms; exclude S < -70 LUFS. Whole-track PLR is a scalar.
    weighted = audio.copy()
    for filt in meter._filters.values():
        for channel in range(2):
            weighted[:, channel] = filt.apply_filter(weighted[:, channel])
    energy = np.sum(weighted ** 2, axis=1)
    sums = np.concatenate(([0.0], np.cumsum(energy)))
    psr = []
    for end in range(30 * hop, len(audio) + 1, hop):
        short = -0.691 + 10 * math.log10(max((sums[end] - sums[end - 30 * hop]) / (30 * hop), 1e-30))
        if short > -70:
            psr.append(db(peaks8[end // hop - 30:end // hop].max()) - short)
    result['psr_median_db'] = float(np.median(psr)) if psr else math.nan
    result['plr_db'] = result['tp8_dbtp'] - loudness
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--render', type=Path, required=True)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--ffmpeg', default='/usr/bin/ffmpeg')
    parser.add_argument('--targets', default='-10,-12,-14')
    parser.add_argument('--jamendo-per-class', type=int, default=0, help='download first N tracks per T-038 category (0..15)')
    parser.add_argument('--synthetic', action='store_true', help='add fixed supplementary stress WAVs to input')
    parser.add_argument('--resume-measurements', action='store_true', help='resume complete CSV conditions after validating inputs/binary')
    parser.add_argument('--reuse-renders', action='store_true', help='reuse existing WAVs; only if settings/source unchanged')
    parser.add_argument('--aac-pns', choices=('default', 'on', 'off'), default='default',
                        help='native AAC PNS override; default leaves ffmpeg options unchanged')
    parser.add_argument('--codecs', choices=('all', 'aac'), default='all',
                        help='select AAC only for supplementary measurements')
    parser.add_argument('--aac-encoder', choices=('aac', 'libfdk_aac', 'aac_at'), default='aac')
    args = parser.parse_args()
    if args.aac_encoder != 'aac' and args.aac_pns != 'default':
        parser.error('--aac-pns only applies to the native aac encoder')
    selected_codecs = tuple((args.aac_encoder if enc == 'aac' else enc, rate, ext)
                            for enc, rate, ext in CODECS if args.codecs == 'all' or enc == 'aac')
    encoder_options = {'aac_pns': args.aac_pns, 'aac_encoder': args.aac_encoder}
    args.input = args.input.resolve()
    args.work = args.work.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    args.work.mkdir(parents=True, exist_ok=True)
    if not 0 <= args.jamendo_per_class <= 15:
        parser.error('--jamendo-per-class must be 0..15')
    if args.jamendo_per_class:
        download_jamendo(args)
    if args.synthetic:
        synthetic(args.input)
    sources = sorted(args.input.glob('*.wav'))
    if not sources:
        parser.error('--input has no WAV files')
    manifest = [{'file': p.name, 'sha256': hashlib.sha256(p.read_bytes()).hexdigest(),
                 'frames': sf.info(p).frames, 'sample_rate': sf.info(p).samplerate} for p in sources]
    if args.resume_measurements and (args.out / 'input_manifest.csv').exists():
        with (args.out / 'input_manifest.csv').open() as stream:
            previous = list(csv.DictReader(stream))
        assert [{k: str(v) for k, v in r.items()} for r in manifest] == previous, 'input cache mismatch'
    write_csv(args.out / 'input_manifest.csv', manifest)
    metadata = {'ffmpeg_version': run([args.ffmpeg, '-version']), 'encoders': selected_codecs, 'encoder_options': encoder_options,
                'encode_options': 'ffmpeg -i MASTER -map 0:a:0 -c:a ENCODER -b:a Nk; Opus: -application audio -vbr on',
                'decode_options': 'ffmpeg -i ENCODED -c:a pcm_f32le DECODED.wav (no gain or limiter)',
                'render_sha256': hashlib.sha256(args.render.read_bytes()).hexdigest(),
                'command': sys.argv, 'render_commands': [],
                'packages': {name: __import__(name).__version__ for name in ('numpy', 'scipy', 'soundfile')},
                'pyloudnorm_version': __import__('importlib.metadata', fromlist=['version']).version('pyloudnorm')}
    metadata['reuse_renders'] = args.reuse_renders
    if args.aac_pns != 'default':
        metadata['encode_options'] += f'; native AAC: -aac_pns {0 if args.aac_pns == "off" else 1}'
    masters, codecs = [], []
    if args.resume_measurements and (args.out / 'environment.json').exists():
        previous = json.loads((args.out / 'environment.json').read_text())
        assert previous['render_sha256'] == metadata['render_sha256'], 'renderer cache mismatch'
        assert previous['ffmpeg_version'] == metadata['ffmpeg_version'], 'ffmpeg cache mismatch'
        assert previous['encoders'] == [list(c) for c in selected_codecs], 'codec cache mismatch'
        assert previous.get('encoder_options', {'aac_pns': 'default', 'aac_encoder': 'aac'}) == encoder_options, 'encoder options cache mismatch'
        metadata['render_commands'] = previous['render_commands']
        for name, records in (('masters.csv', masters), ('codec_roundtrips.csv', codecs)):
            if (args.out / name).exists():
                with (args.out / name).open() as stream:
                    for row in csv.DictReader(stream):
                        records.append({k: v if k in ('file', 'encoder') else float(v) for k, v in row.items()})
    (args.out / 'environment.json').write_text(json.dumps(metadata, indent=2, ensure_ascii=False) + '\n')
    for target in (float(x) for x in args.targets.split(',')):
        output = args.work / f'render_{target:g}'
        command = [args.render.resolve(), '--in', args.input, '--out', output, '--glaze', '100',
                   '--style', 'auto', '--clay', 'auto', '--genre', 'general', '--delivery', 'streaming',
                   '--streaming-target-lufs', str(target), '--no-align', '--no-replay']
        metadata['render_commands'].append([str(x) for x in command])
        if not args.reuse_renders:
            run(command, args.work / f'render_{target:g}.log')
        files = sorted(output.glob('*.wav'))
        if len(files) != len(sources):
            raise ValueError(f'{output}: expected {len(sources)} masters, got {len(files)}')
        for path in files:
            existing = [r for r in codecs if r['file'] == path.name and r['target_lufs'] == target]
            if args.resume_measurements and len(existing) == len(selected_codecs):
                continue
            # Replace any incomplete condition checkpoint.
            masters = [r for r in masters if not (r['file'] == path.name and r['target_lufs'] == target)]
            codecs = [r for r in codecs if not (r['file'] == path.name and r['target_lufs'] == target)]
            print(f'{target:g}: {path.name}', flush=True)
            pre = measure(path)
            key = {'file': path.name, 'target_lufs': target}
            masters.append({**key, **pre})
            for encoder, bitrate, extension in selected_codecs:
                suffix = f'_pns_{args.aac_pns}' if encoder == 'aac' and args.aac_pns != 'default' else ''
                encoded = args.work / f'{path.stem}_{target:g}_{encoder}_{bitrate}{suffix}.{extension}'
                decoded = args.work / (encoded.name + '.wav')
                command = [args.ffmpeg, '-v', 'error', '-nostdin', '-y', '-i', path,
                           '-map', '0:a:0', '-c:a', encoder, '-b:a', f'{bitrate}k']
                if encoder == 'aac' and args.aac_pns != 'default':
                    command += ['-aac_pns', '0' if args.aac_pns == 'off' else '1']
                if encoder == 'libopus':
                    command += ['-application', 'audio', '-vbr', 'on']
                run(command + [encoded])
                run([args.ffmpeg, '-v', 'error', '-nostdin', '-y', '-i', encoded,
                     '-c:a', 'pcm_f32le', decoded])  # float prevents silently clipping decoded peaks.
                post = measure(decoded)
                row = {**key, 'encoder': encoder, 'bitrate_kbps': bitrate,
                       **{'pre_' + k: v for k, v in pre.items()},
                       **{'post_' + k: v for k, v in post.items()},
                       'overshoot4_db': post['tp4_dbtp'] - pre['tp4_dbtp'],
                       'overshoot8_db': post['tp8_dbtp'] - pre['tp8_dbtp']}
                # Requested platform gain-only models, attenuation only; no platform upload.
                for platform, level in (('spotify', -14), ('youtube', -14), ('apple', -16)):
                    gain = min(0, level - post['lufs_i'])
                    row[platform + '_attenuation_db'] = -gain
                    row[platform + '_playback_tp_dbtp'] = post['tp8_dbtp'] + gain
                codecs.append(row)
                decoded.unlink()  # Keep encoded artifacts and unaligned masters locally.
            write_csv(args.out / 'masters.csv', masters)
            write_csv(args.out / 'codec_roundtrips.csv', codecs)
    (args.out / 'environment.json').write_text(json.dumps(metadata, indent=2, ensure_ascii=False) + '\n')


if __name__ == '__main__':
    main()
