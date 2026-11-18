/* SPDX-License-Identifier: GPL-3.0-only */
#include "phrase.h"
Power rhythm_power(ta_context *c, ta_phrase_biquad *section, D input, double rate, double bpm,
                   I coordinates, int n, bool low) {
    D source = section ? filter(c, section, 1, input, false) : input;
    double period = rate * 60 / bpm;
    int quarter = (int)(period * 0.25 + 0.5);
    Power p = {ta_new_doubles(c, n * 4), ta_new_doubles(c, n * 4), ta_new_doubles(c, n / 4)};
    for (int beat = 0; beat < n; beat++) {
        int start = coordinates.v[beat], end = beat + (low ? 2 : 1) < n
                                                   ? coordinates.v[beat + 1]
                                                   : start + (int)(period + 0.5) - 1;
        for (int part = 0; part < 4; part++) {
            int first = start - 1 + quarter * part, last = part == 3 ? end - 1 : first + quarter;
            if (first < 0 || last < first || last > source.n)
                continue;
            p.quarters.v[beat * 4 + part] = d_square(d_view(c, source, first, last));
            double peak = 0;
            for (int i = first; i < last; i++)
                peak = ta_dmax(peak, fabs(source.v[i]));
            p.peaks.v[beat * 4 + part] = peak;
        }
    }
    if (low && !(rate > 5512.45 && rate < 5512.55))
        for (int i = 0; i < p.quarters.n; i++)
            p.quarters.v[i] *= 4;
    for (int bar = 0; bar < p.bars.n; bar++)
        p.bars.v[bar] = d_sequential(d_view(c, p.quarters, bar * 16, bar * 16 + 16));
    if (section)
        ta_release(c, source.v);
    return p;
}
Attacks attacks_init(ta_context *c, double rate, bool second) {
    const double damping[] = {1.8477590650225735, 0.7653668647301797, 1.8477590650225735,
                              0.7653668647301797};
    bool full = fabs(rate - 5512.5) < 0.01 || fabs(rate - 6000) < 0.01 || fabs(rate - 4000) < 0.01;
    bool reduced =
        fabs(rate - 1378.125) < 0.01 || fabs(rate - 1500) < 0.01 || fabs(rate - 1000) < 0.01;
    if (!full && !reduced)
        ta_fail(c, TA_FAILED, "unsupported rhythm rate");
    Attacks a = {.lowCount = full ? 4 : 1, .second = second, .maximum = 0.0001};
    for (int i = 0; i < a.lowCount; i++)
        a.low[i] =
            second_order(rate, 1256.6370614359173, full ? damping[i] : 1.4142135623730951, false);
    for (int i = 0; i < 4; i++)
        a.high[i] = second_order(rate, 691.1503837897545, damping[i], true);
    a.delaySmoother = second_order(rate, 62.83185307179586, 1.4142135623730951, false);
    a.bandSmoother = a.delaySmoother;
    return a;
}
static int find_delay(ta_context *c, D envelope, int start, double period, int bars, bool ascending,
                      bool rounded) {
    if (bars <= 1)
        return 0;
    int barLength = (int)(period * 4), quarter = (int)(period * 0.25 + (rounded ? 0.5 : 0)),
        base = start - 1, length = (bars - 1) * barLength;
    if (base + barLength < 0 || base + barLength + length > envelope.n || length <= 0)
        return 0;
    D search = d_copy(c, d_view(c, envelope, base + barLength, base + barLength + length));
    int peakPosition = 0, valleyPosition = 0, result = 0;
    double peak = 0;
    for (int pass = 0; pass < 8; pass++) {
        ta_check(c);
        int maxIndex = 0;
        for (int i = 0; i < search.n; i++)
            if (search.v[i] > search.v[maxIndex])
                maxIndex = i;
        double maximum = search.v[maxIndex];
        search.v[maxIndex] = 0;
        int bar = (int)((double)(maxIndex + 1) * 0.25 * (1 / period)),
            boundary = barLength + bar * barLength, position = base + boundary;
        if (position - 1 < 0 || position + barLength > envelope.n ||
            boundary >= length - barLength || envelope.v[position - 1] <= maximum * 0.2)
            continue;
        bool rising = envelope.v[position - 1] <= envelope.v[position] &&
                      envelope.v[position] <= envelope.v[position + 1] &&
                      envelope.v[position + 1] <= envelope.v[position + 2];
        bool falling = envelope.v[position - 1] > envelope.v[position] &&
                       envelope.v[position] > envelope.v[position + 1] &&
                       envelope.v[position + 1] > envelope.v[position + 2];
        if (rising) {
            for (int step = 0; step < quarter; step++)
                if (position + step + 1 < envelope.n &&
                    envelope.v[position + step + 1] - envelope.v[position + step] < 0) {
                    peakPosition = boundary + step + 1;
                    peak = envelope.v[base - 1 + peakPosition];
                    break;
                }
        } else if (falling && !ascending) {
            for (int step = 0; step < quarter; step++)
                if (position - 3 - step >= 0 &&
                    envelope.v[position - 3 - step] - envelope.v[position - 2 - step] < 0) {
                    peakPosition = boundary - step - 1;
                    peak = envelope.v[base - 1 + peakPosition];
                    break;
                }
        } else
            continue;
        if (peak <= maximum * 0.595662)
            continue;
        double minimum = d_min(d_view(c, envelope, position - 1, position + barLength)),
               threshold = (minimum + maximum) * 0.5, valley = maximum;
        bool found = false;
        int run = 0;
        for (int step = 1; step <= ta_max(1, (int)(period * 1.25)); step++) {
            int relative = peakPosition - step, index = start + relative - 2;
            if (index <= 0 || index >= envelope.n)
                break;
            if (found) {
                if (envelope.v[index] <= valley * 1.12) {
                    if (++run >= (int)(period * 0.0625)) {
                        if (valley == 0)
                            goto done;
                        if (peak / valley > 2.11) {
                            result = ta_min(start,
                                            (int)((double)boundary + 0.5 - (double)valleyPosition));
                            goto done;
                        }
                        break;
                    }
                } else {
                    found = false;
                    run = 0;
                }
            } else if (envelope.v[index] <= envelope.v[index - 1] &&
                       envelope.v[index] < threshold) {
                found = true;
                valley = envelope.v[index];
                valleyPosition = relative;
            }
        }
    }
done:
    ta_release(c, search.v);
    return result;
}
B attacks_process(ta_context *c, Attacks *a, D input, double rate, double bpm, int offset,
                  int phase, int block, I coordinates, int n, bool integerPeriod) {
    if (!input.n || n <= 0)
        return ta_new_bytes(c, 1);
    double period = integerPeriod ? (double)(int)(rate * 60 / bpm + 0.5) : rate * 60 / bpm;
    int start = (int)((double)((int)(rate * (double)offset * 0.001 + 0.5) + 1) +
                      (double)(phase - 1) * period),
        bars = (n - phase + 1) / 4, barLength = (int)(period * 4);
    if (input.n < start - 1 + bars * barLength)
        bars--;
    D filtered = filter(c, a->low, a->lowCount, input, false),
      lowEnvelope = filter(c, &a->delaySmoother, 1, filtered, true);
    if (block < 2)
        a->maximum = 0.0001;
    if (bars <= 0) {
        a->previous = lowEnvelope.n ? lowEnvelope.v[lowEnvelope.n - 1] : 0;
        ta_release(c, filtered.v);
        ta_release(c, lowEnvelope.v);
        return ta_new_bytes(c, 1);
    }
    int oldDelay = a->delay;
    a->delay = find_delay(c, lowEnvelope, start, period, bars, a->second, integerPeriod);
    int activeDelay = block < 2 ? a->delay : oldDelay;
    if (integerPeriod && a->second)
        a->delay = oldDelay;
    D storage = lowEnvelope;
    if (a->second) {
        D band = filter(c, a->high, 4, filtered, false);
        storage = filter(c, &a->bandSmoother, 1, band, true);
        ta_release(c, band.v);
        ta_release(c, lowEnvelope.v);
    }
    ta_release(c, filtered.v);
    double previous = a->previous;
    if (!integerPeriod || a->second)
        a->previous = storage.v[storage.n - 1];
    for (int i = storage.n - 1; i >= 1; i--)
        storage.v[i] -= storage.v[i - 1];
    storage.v[0] -= previous;
    int end = coordinates.v[n - 1], proposed = end + (int)period - 1;
    if (proposed <= input.n)
        end = proposed;
    D envelope = storage;
    if (activeDelay < start && end - activeDelay <= storage.n)
        envelope = d_view(c, storage, start - 1 - activeDelay, end - activeDelay);
    else if (start < activeDelay && end - activeDelay - 1 <= storage.n) {
        int prefix = ta_max(0, end - activeDelay - 1), zeros = activeDelay - (start - 1),
            outCount = ta_max(storage.n, end - start);
        envelope = ta_new_doubles(c, ta_max(zeros + prefix, outCount));
        if (prefix)
            memcpy(envelope.v + zeros, storage.v, prefix * sizeof(double));
        ta_release(c, storage.v);
        storage = envelope;
    }
    B output = ta_new_bytes(c, bars * 16);
    if (integerPeriod)
        a->maximum = 0.0001;
    int quarter = (int)(period * 0.25 + (integerPeriod ? 0.5 : 0));
    for (int bar = 0; bar < bars; bar++) {
        ta_check(c);
        double peaks[16] = {0};
        for (int beat = 0; beat < 4; beat++) {
            int ordinal = phase - 1 + bar * 4 + beat;
            if (ordinal >= coordinates.n)
                goto done;
            double base = integerPeriod ? (double)(bar * 4 + beat) * period
                                        : (double)(coordinates.v[ordinal] - start) + 0.5;
            for (int part = 0; part < 4; part++) {
                int first = (int)(base + period * ((double)part * 0.25));
                if (first < 0 || first + quarter > envelope.n)
                    goto done;
                peaks[beat * 4 + part] = d_max((D){envelope.v + first, quarter});
            }
        }
        double maximum = d_max((D){peaks, 16});
        for (int pass = 0; pass < (a->second ? 12 : 6); pass++) {
            double threshold = d_max((D){peaks, 16});
            if (threshold > 0.0001 && threshold > maximum * (a->second ? 0.5 : 0.596) &&
                threshold > a->maximum * 0.178) {
                int index = 0;
                for (int i = 1; i < 16; i++)
                    if (peaks[i] > peaks[index])
                        index = i;
                output.v[bar * 16 + index] = 1;
                peaks[index] = 0;
            }
        }
        a->maximum = ta_dmax(a->maximum, maximum);
    }
done:
    ta_release(c, storage.v);
    return output;
}
B attacks_timing(ta_context *c, Attacks *a, D input, double rate, double bpm, int offset, int phase,
                 int block) {
    int period = (int)(rate * 60 / bpm + 0.5);
    if (period <= 0)
        return ta_new_bytes(c, 1);
    int first = (int)(rate * (double)offset * 0.001 + 0.5),
        n = ta_max(0, (input.n - first - (phase - 1) * period) / period + phase - 1);
    I positions = ta_new_ints(c, n);
    for (int i = 0; i < n; i++)
        positions.v[i] = first + 1 + i * period;
    B result = attacks_process(c, a, input, rate, bpm, offset, phase, block, positions, n, true);
    ta_release(c, positions.v);
    return result;
}
static B hihat_counts(ta_context *c, D s, double rate, double bpm, I positions, int n) {
    B result = ta_new_bytes(c, n);
    double fast = 0, slow = 0, reciprocal = 1 / rate, gap = rate * 13.333333333333332 / bpm;
    int previous = 0;
    for (int i = 0; i < s.n; i++) {
        if (!(i & 16383))
            ta_check(c);
        double value = fabs(s.v[i]);
        fast = fma((value < fast ? 20 : 2000) * (value - fast), reciprocal, fast);
        slow = fma((value < slow ? 13.333333333333334 : 1000) * (value - slow), reciprocal, slow);
        if (slow < fast && fast > 0.06 && gap < (double)(i + 1 - previous)) {
            previous = i + 1;
            for (int beat = 0; beat < n; beat++)
                if (positions.v[beat] <= i + 1 && (beat == n - 1 || i + 1 < positions.v[beat + 1]))
                    result.v[beat]++;
        }
    }
    return result;
}
static B hihat_changes(ta_context *c, B counts, int phase) {
    int remaining = counts.n - phase + 1;
    if (remaining <= 0)
        return ta_new_bytes(c, 0);
    int bars = (remaining + 3) / 4;
    B levels = ta_new_bytes(c, bars), changes = ta_new_bytes(c, bars);
    for (int i = 0; i < remaining; i++)
        levels.v[i / 4] += counts.v[phase - 1 + i];
    for (int i = 0; i < bars; i++)
        levels.v[i] = levels.v[i] < 3 ? 0 : levels.v[i] < 7 ? 4 : levels.v[i] < 12 ? 8 : 16;
    for (int length = 1; length <= 3; length++)
        if (bars >= length + 2) {
            bool changed = true;
            while (changed) {
                ta_check(c);
                changed = false;
                for (int i = 1; i < bars - length; i++) {
                    int level = levels.v[i - 1];
                    bool different = true;
                    for (int j = i; j < i + length; j++)
                        if (levels.v[j] == level)
                            different = false;
                    if (level == levels.v[i + length] && different) {
                        for (int j = i; j < i + length; j++)
                            levels.v[j] = level;
                        changed = true;
                    }
                }
            }
        }
    for (int i = 1; i < bars; i++)
        if (levels.v[i] != levels.v[i - 1])
            changes.v[i] = 1;
    ta_release(c, levels.v);
    return changes;
}
static D divided_band(ta_context *c, D samples, double rate, bool high, I positions, int n) {
    ta_phrase_biquad band = second_order(rate, 3141.592653589793, 1.4142135623730951, high),
                     smooth = smoother(rate, 31.41592653589793);
    D tmp = filter(c, &band, 1, samples, false), envelope = filter(c, &smooth, 1, tmp, true),
      result = ta_new_doubles(c, n);
    ta_release(c, tmp.v);
    for (int i = 0; i < n; i++) {
        int first = positions.v[i] - 1, end = i + 1 < n ? positions.v[i + 1] - 1 : envelope.n;
        if (first >= 0 && end > first && end <= envelope.n)
            result.v[i] = d_sum(d_view(c, envelope, first, end));
    }
    ta_release(c, envelope.v);
    return result;
}
static void release_power(ta_context *c, Power p) {
    ta_release(c, p.quarters.v);
    ta_release(c, p.peaks.v);
    ta_release(c, p.bars.v);
}
Rhythm phrase_rhythm(ta_context *c, Samples s, Timing t, Coordinates coordinates) {
    int count = coordinates.n, bars = ta_max(0, (count - t.phase + 1) / 4), block = 0, lowEnd = 0,
        midEnd = 0;
    double lowRate = s.rate / 32, midRate = s.rate / 8, highRate = s.rate / 2;
    ta_phrase_biquad low = second_order(lowRate, 628.3185307179587, 1.4142135623730951, false),
                     sub[4];
    for (int i = 0; i < 4; i++)
        sub[i] = second_order(lowRate, 251.32741228718345, 1.4142135623730951, false);
    Attacks first = attacks_init(c, midRate, false), second = attacks_init(c, midRate, true);
    Rhythm r = {.lowPower = ta_new_doubles(c, count * 4),
                .subPower = ta_new_doubles(c, count * 4),
                .allPower = ta_new_doubles(c, count * 4),
                .allPeaks = ta_new_doubles(c, count * 4),
                .first = ta_new_bytes(c, bars * 16),
                .second = ta_new_bytes(c, bars * 16),
                .hiHat = ta_new_bytes(c, count),
                .finalMiddle = i_copy(c, coordinates.middle)};
    for (int begin = 0; begin < count;) {
        ta_check(c);
        int phase = begin ? 1 : t.phase, offset = begin ? 0 : t.offset,
            nominal = nominal_beats(60 / t.bpm, phase, (double)offset * 0.001);
        if (nominal <= 0)
            ta_fail(c, TA_FAILED, "invalid rhythm block timing");
        int end = ta_min(count, begin + nominal), n = end - begin;
        bool partial = nominal > count - begin;
        block++;
        D sources[] = {s.low, s.middle, s.high}, regions[3];
        I allpos[] = {coordinates.low, coordinates.middle, coordinates.high}, pos[3];
        for (int band = 0; band < 3; band++) {
            int start = begin ? allpos[band].v[begin] - 1 : 0,
                stop = partial ? sources[band].n : allpos[band].v[end] - 1;
            regions[band] = d_view(c, sources[band], start, stop);
            pos[band] = ta_new_ints(c, n + 1);
            for (int i = 0; i <= n; i++)
                pos[band].v[i] = allpos[band].v[begin + i] - start;
            if (!band)
                lowEnd = stop;
            if (band == 1)
                midEnd = stop;
        }
        if (!r.zeroData)
            for (int i = 0; i < regions[0].n; i++)
                if (regions[0].v[i] != 0) {
                    r.zeroData = 1;
                    break;
                }
        Power lp = rhythm_power(c, &low, regions[0], lowRate, t.bpm, pos[0], n, true);
        D subFiltered = filter(c, sub, 4, regions[0], false);
        Power sp = rhythm_power(c, NULL, subFiltered, lowRate, t.bpm, pos[0], n, false);
        ta_release(c, subFiltered.v);
        if (!(lowRate > 5512.45 && lowRate < 5512.55))
            for (int i = 0; i < sp.quarters.n; i++)
                sp.quarters.v[i] *= 4;
        Power ap = rhythm_power(c, NULL, regions[1], midRate, t.bpm, pos[1], n, false);
        memcpy(r.lowPower.v + begin * 4, lp.quarters.v, n * 4 * sizeof(double));
        memcpy(r.subPower.v + begin * 4, sp.quarters.v, n * 4 * sizeof(double));
        memcpy(r.allPower.v + begin * 4, ap.quarters.v, n * 4 * sizeof(double));
        memcpy(r.allPeaks.v + begin * 4, ap.peaks.v, n * 4 * sizeof(double));
        release_power(c, lp);
        release_power(c, sp);
        release_power(c, ap);
        B a = attacks_process(c, &first, regions[1], midRate, t.bpm, offset, phase, block, pos[1],
                              n, false),
          b = attacks_process(c, &second, regions[1], midRate, t.bpm, offset, phase, block, pos[1],
                              n, false);
        int completed = (end - t.phase + 1) / 4;
        B values[] = {a, b};
        B *dest[] = {&r.first, &r.second};
        for (int k = 0; k < 2; k++) {
            int start = completed * 16 - values[k].n;
            if (start < 0 || completed < 1) {
                dest[k]->n = 1;
                dest[k]->v[0] = 0;
            } else {
                if (start + values[k].n > dest[k]->n)
                    ta_fail(c, TA_FAILED, "rhythm attack range");
                memcpy(dest[k]->v + start, values[k].v, values[k].n);
            }
            ta_release(c, values[k].v);
        }
        B h = hihat_counts(c, regions[2], highRate, t.bpm, pos[2], n);
        memcpy(r.hiHat.v + begin, h.v, n);
        ta_release(c, h.v);
        for (int band = 0; band < 3; band++)
            ta_release(c, pos[band].v);
        if (end < count) {
            int origin = r.finalMiddle.v[n];
            for (int i = 0; i < r.finalMiddle.n; i++)
                r.finalMiddle.v[i] -= origin;
            for (int i = 0; i < r.finalMiddle.n - n; i++)
                r.finalMiddle.v[i] = r.finalMiddle.v[i + n] + 1;
        }
        begin = end;
    }
    r.dividedLow =
        divided_band(c, d_view(c, s.low, 0, lowEnd), lowRate, false, coordinates.low, count);
    r.dividedMiddle =
        divided_band(c, d_view(c, s.middle, 0, midEnd), midRate, true, coordinates.middle, count);
    r.middleExtent = midEnd;
    r.hiHatChanges = hihat_changes(c, r.hiHat, t.phase);
    return r;
}
