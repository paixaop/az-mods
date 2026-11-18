/* SPDX-License-Identifier: GPL-3.0-only */
#include "phrase.h"
int initial_mood(MoodInputs x) {
    if (!(x.lowPowerRatio >= 0.3979) || x.slowRock == 1 ||
        !(x.compression >= 0.6128 || x.slowTempo != 1))
        return MOOD_LOW;
    if (x.fadeOut == 1 || x.firstOverride == 1)
        return MOOD_MID;
    if (x.secondOverride == 1)
        return MOOD_HIGH;
    double score = (double)x.alternateRock * -0.05;
    score = fma((double)x.narrowBPM - (double)x.sparseKick, 0.3, score);
    score = fma((double)x.typicalGenre + (double)x.broadBPM, 0.1, score);
    score = fma((double)x.fourKick - (double)x.popRock + x.compression, 0.6, score);
    return score > 1 ? MOOD_HIGH : MOOD_MID;
}
void bpm_flags(double bpm, int *narrow, int *broad) {
    *narrow = (bpm >= 127.04 && bpm <= 128.96) || (bpm >= 132.330025 && bpm <= 134.32997500000002);
    *broad = !*narrow && ((bpm >= 122.88 && bpm <= 133.12) ||
                          (bpm >= 127.99680000000001 && bpm <= 138.66320000000002));
}
LowPower classify_power(ta_context *c, D low, D full, int beats) {
    if (low.n != full.n || beats < 0 || low.n < (beats / 4) * 16)
        ta_fail(c, TA_FAILED, "power dimensions");
    int n = beats / 4;
    double den = d_sum(full);
    LowPower p = {.ratio = den > 0 ? sqrt(d_sum(low) / den) : 1,
                  .ratios = ta_new_doubles(c, ta_max(1, n)),
                  .powers = ta_new_doubles(c, ta_max(1, n))};
    for (int b = 0; b < n; b++) {
        double a = d_sequential(d_view(c, low, b * 16, b * 16 + 16)),
               d = d_sequential(d_view(c, full, b * 16, b * 16 + 16));
        p.powers.v[b] = a;
        p.ratios.v[b] = d > 0 ? a / d : 0;
    }
    if (n > 0 && beats < 128) {
        p.ratioMean = d_sum(p.ratios) / (double)n;
        p.powerMean = d_sum(p.powers) / (double)n;
    } else if (beats >= 128)
        for (int w = 0; w < beats / 64 - 1; w++) {
            p.ratioMean = ta_dmax(p.ratioMean,
                                  d_sequential(d_view(c, p.ratios, w * 16, w * 16 + 16)) * 0.0625);
            p.powerMean = ta_dmax(p.powerMean,
                                  d_sequential(d_view(c, p.powers, w * 16, w * 16 + 16)) * 0.0625);
        }
    return p;
}
Compression classify_compression(ta_context *c, D low, D full, B rhythm, int phase) {
    if (low.n != full.n || phase < 1)
        ta_fail(c, TA_FAILED, "compression dimensions");
    int offset = (phase - 1) * 4, blocks = (rhythm.n - offset) / 128;
    if (rhythm.n - offset < 128)
        return (Compression){0.1, 0.3};
    if (offset + blocks * 128 > full.n)
        ta_fail(c, TA_FAILED, "compression range");
    double maximum = 0, selectedMax = 0;
    for (int b = 0; b < blocks; b++) {
        double selectedLow = 0, selectedFull = 0;
        int hits = 0, previous = -1;
        for (int slot = 0; slot < 128; slot++)
            if (rhythm.v[b * 128 + slot] == 1) {
                hits++;
                int index = offset + b * 128 + slot;
                if (index != previous && index < full.n) {
                    selectedLow += low.v[index];
                    selectedFull += full.v[index];
                }
                if (index + 1 < full.n) {
                    selectedLow += low.v[index + 1];
                    selectedFull += full.v[index + 1];
                    previous = index + 1;
                } else
                    previous = -1;
            }
        if (hits < 4)
            continue;
        double total = d_sum(d_view(c, full, offset + b * 128, offset + b * 128 + 128));
        double off = hits < 128 ? (total - selectedFull) / (double)(128 - hits) : 0,
               on = selectedFull / (double)hits;
        maximum = ta_dmax(maximum, off + on > 0 ? off / (off + on) : 0);
        selectedMax = ta_dmax(selectedMax, selectedLow / (double)hits);
    }
    return (Compression){maximum, selectedMax * 0.0075175301};
}
int slow_rock(ta_context *c, B low, B middle) {
    if (middle.n < low.n)
        ta_fail(c, TA_FAILED, "slow rock dimensions");
    int previous = 0, matches = 0;
    for (int start = 0; start < low.n - 32; start++) {
        if (low.v[start] != 1 || middle.v[start] != 1)
            continue;
        bool ok = true, found = false;
        for (int i = 2; i <= 6; i++)
            if (low.v[start + i] || middle.v[start + i])
                ok = false;
        for (int i = 7; i <= 9; i++)
            if (!low.v[start + i] && middle.v[start + i] == 1)
                found = true;
        if (!ok || !found)
            continue;
        for (int i = 10; i <= 14; i++)
            if (low.v[start + i] || middle.v[start + i])
                ok = false;
        found = false;
        for (int i = 15; i <= 17; i++)
            if (low.v[start + i] == 1 && middle.v[start + i] == 1)
                found = true;
        if (!ok || !found)
            continue;
        if (start == previous || start + 1 == previous || start + 2 == previous) {
            matches++;
            previous = 0;
        } else
            previous = start + 17;
    }
    return matches > 1;
}
RhythmFlags rhythm_flags(ta_context *c, B rhythm, double bpm, D powers) {
    if (!powers.n)
        ta_fail(c, TA_FAILED, "rhythm flags dimensions");
    double mean = d_sum(powers) / (double)powers.n;
    int start = powers.n < 17 ? 1 : 17;
    for (int i = start; i <= powers.n - 3; i++)
        if (powers.v[i - 1] > mean) {
            start = i;
            break;
        }
    int remaining = (int)((double)rhythm.n * 0.063) - start, groups = ta_min(8, remaining / 8);
    RhythmFlags f = {0};
    if (groups <= 0)
        return f;
    if ((start - 1 + groups * 8) * 16 > rhythm.n)
        ta_fail(c, TA_FAILED, "rhythm flags range");
    const unsigned masks[] = {0x1111, 0x4444, 0x141, 0x101, 0x501};
    const int offsets[] = {1, 2, 5, 6};
    int counts[4] = {0};
    for (int group = 0; group < groups; group++) {
        bool found[4] = {0};
        for (int j = 0; j < 4; j++) {
            int bar = start - 1 + group * 8 + offsets[j];
            if (bar <= 0)
                continue;
            bool matches[5] = {true, true, true, true, true};
            for (int p = 0; p < 5; p++)
                for (int k = 0; k < 16; k++)
                    if (rhythm.v[bar * 16 + k] != ((masks[p] >> k) & 1u))
                        matches[p] = false;
            found[0] |= matches[0] || matches[1];
            found[1] |= matches[2];
            found[2] |= matches[3];
            found[3] |= matches[4];
        }
        for (int i = 0; i < 4; i++)
            if (found[i])
                counts[i]++;
    }
    int required = remaining > 63 ? 1 : 0;
    if (counts[0] >= 2)
        f.fourKick = 1;
    else if (counts[3] > required)
        f.sparseKick = 1;
    else if (counts[1] > required && bpm < 165)
        f.popRock = 1;
    else if (counts[2] > required)
        f.alternateRock = 1;
    return f;
}
int slow_tempo(ta_context *c, double rate, double bpm, D powers, SnarePositions onsets, I beats,
               int sourceOffset, int analysisOffset) {
    if (onsets.first < 1 || onsets.count < 0 || onsets.first - 1 + onsets.count > onsets.values.n ||
        powers.n > beats.n / 4)
        ta_fail(c, TA_FAILED, "slow tempo dimensions");
    D onset = ta_new_doubles(c, onsets.values.n);
    for (int i = 0; i < onset.n; i++)
        onset.v[i] = (double)onsets.values.v[i];
    int last = onsets.first + onsets.count;
    double shift = rate * 0.001 * (double)(sourceOffset - analysisOffset);
    for (int i = onsets.first - 1; i < last - 1; i++)
        onset.v[i] += shift;
    double period = bpm == 0 ? rate * 0.5 : rate * 60 / bpm;
    int rounded = (int)(period + 0.5);
    if (!rounded)
        ta_fail(c, TA_FAILED, "slow tempo period");
    double quarter = (double)(int)(period * 0.25 + 0.5),
           mean = powers.n ? d_sum(powers) / (double)powers.n : 0;
    int votes = 0, yes = 0;
    if (onsets.first <= last - 2)
        for (int bar = 0; bar < powers.n - 3; bar++)
            if (powers.v[bar] > mean) {
                double start = beats.v[bar * 4], end = beats.v[ta_min(bar * 4 + 31, beats.n - 1)];
                int i = onsets.first - 1;
                do {
                    double t = onset.v[i];
                    if (t >= start && t <= end && i + 2 < last) {
                        double divided = (t - start) * (1 / (double)rounded),
                               position = divided - (double)((int)divided * rounded);
                        bool inside = position >= quarter && position <= quarter * 3;
                        double gap = onset.v[i + 1] - t;
                        if (gap > period * 1.875 && gap < period * 2.125 && !inside && votes < 64)
                            votes++;
                        if (gap > period * 0.875 && gap < period * 1.125 && inside && votes < 64) {
                            votes++;
                            yes++;
                        }
                    }
                    i++;
                    if (t > end)
                        break;
                } while (i < last - 2);
                if (votes >= 64)
                    break;
            }
    bool sync = votes && (double)yes / (double)votes > 0.5;
    ta_release(c, onset.v);
    return !sync && bpm < 83.9;
}
int fade_out(D low, D full) {
    if (low.n < 1552 || full.n < low.n)
        return 0;
    int start = (low.n & ~15) - 528;
    double lb[32], fb[32], pairs[16], ratios[15] = {0};
    bool silent = false;
    for (int i = 0; i < 32; i++) {
        lb[i] = d_sequential((D){low.v + start + i * 16, 16});
        fb[i] = d_sequential((D){full.v + start + i * 16, 16});
    }
    for (int i = 0; i < 16; i++) {
        pairs[i] = fb[i * 2] + fb[i * 2 + 1];
        if (pairs[i] < 1e-7)
            silent = true;
    }
    if (!silent)
        for (int i = 0; i < 15; i++)
            ratios[i] = (lb[i * 2] + lb[i * 2 + 1]) / pairs[i];
    double min = d_min((D){ratios, 12}), max = d_max((D){ratios, 12});
    return pairs[15] < d_max((D){pairs, 16}) * 0.001 && pairs[15] < pairs[14] &&
           pairs[14] < pairs[13] && pairs[13] < pairs[12] && ratios[12] >= min &&
           ratios[12] <= max && ratios[13] >= min && ratios[13] <= max && ratios[14] >= min &&
           ratios[14] <= max;
}
int revise_mood(ta_context *c, int mood, int beats, D low, D quarters) {
    if (mood != MOOD_MID || beats < 96)
        return mood;
    int from = 8, to = beats - 64, count = (to * 4 - from * 4 - 1 - 32) / 4;
    double mean = d_sequential(d_view(c, low, from - 1, to - 1)) / (double)(to - from);
    if (count <= 6)
        return mood;
    D maxima = ta_new_doubles(c, count), correspondence = ta_new_doubles(c, count);
    int previous = 0;
    for (int bar = 1; bar < count; bar++) {
        int first = from * 4 + (bar - 1) * 4;
        D values = d_view(c, quarters, first, first + 4);
        int index = 0;
        for (int i = 1; i < 4; i++)
            if (values.v[i] > values.v[index])
                index = i;
        int position = index + 1, delta = abs(position - previous);
        double maximum = values.v[index];
        maxima.v[bar] = maximum;
        if (!delta)
            correspondence.v[bar - 1] = 1;
        else if (delta == 1 || delta == 3)
            correspondence.v[bar - 1] =
                quarters.v[first - 1 + previous] / (maximum == 0 ? 0.003 : maximum);
        previous = position;
    }
    if (d_sequential(correspondence) / (double)count > 0.91) {
        double changes = 0;
        for (int bar = 2; bar < count - 4; bar++) {
            if (maxima.v[bar] == 0)
                maxima.v[bar] = 0.003;
            if (maxima.v[bar - 1] == 0)
                maxima.v[bar - 1] = 0.003;
            changes += ta_dmax(maxima.v[bar], maxima.v[bar - 1]) /
                       ta_dmin(maxima.v[bar], maxima.v[bar - 1]);
        }
        if (changes / (double)(count - 6) >= 1.5 && mean >= 0.2)
            mood = MOOD_HIGH;
    }
    ta_release(c, maxima.v);
    ta_release(c, correspondence.v);
    return mood;
}
Recurrence recurrence(ta_context *c, Spectra spectra, I boundaries, int phase, int endBeat) {
    if (!spectra.n || spectra.rows[0].n <= 0 || !boundaries.n)
        ta_fail(c, TA_FAILED, "recurrence dimensions");
    int n = boundaries.n, width = spectra.rows[0].n;
    I indices = ta_new_ints(c, n + 1);
    D means = ta_new_doubles(c, spectra.n);
    for (int i = 0; i <= n; i++) {
        indices.v[i] =
            ta_max(1, (int)((double)((i < n ? boundaries.v[i] : endBeat) - phase) * 0.5 + 1));
        if (i && indices.v[i] < indices.v[i - 1])
            ta_fail(c, TA_FAILED, "recurrence boundaries");
    }
    for (int i = 0; i < spectra.n; i++) {
        if (spectra.rows[i].n != width)
            ta_fail(c, TA_FAILED, "ragged recurrence spectra");
        means.v[i] = d_sum(spectra.rows[i]) / (double)width;
    }
    Recurrence r = {
        .scores = ta_alloc(c, n, sizeof(D)), .related = ta_alloc(c, n, sizeof(B)), .n = n};
    D positives = ta_new_doubles(c, n * n);
    positives.n = 0;
    for (int i = 0; i < n; i++) {
        r.scores[i] = ta_new_doubles(c, n);
        r.related[i] = ta_new_bytes(c, n);
        for (int j = i + 1; j < n; j++) {
            ta_check(c);
            int count =
                ta_min(ta_min(indices.v[i + 1] - indices.v[i], indices.v[j + 1] - indices.v[j]),
                       spectra.n - indices.v[j]);
            if (count <= 0)
                continue;
            double total = 0;
            for (int k = 0; k < count; k++) {
                int a = indices.v[i] + k, b = indices.v[j] + k, full = width & ~7;
                double lanes[8] = {0};
                for (int col = 0; col < full; col++) {
                    double delta = (means.v[b] + spectra.rows[a].v[col]) -
                                   (means.v[a] + spectra.rows[b].v[col]);
                    lanes[col & 7] = fma(delta, delta, lanes[col & 7]);
                }
                double distance = (lanes[6] + (lanes[4] + (lanes[2] + lanes[0]))) +
                                  (lanes[7] + (lanes[5] + (lanes[3] + lanes[1])));
                for (int col = full; col < width; col++) {
                    double delta = (means.v[b] + spectra.rows[a].v[col]) -
                                   (means.v[a] + spectra.rows[b].v[col]);
                    distance = fma(delta, delta, distance);
                }
                total += distance == 0 ? 65535 : 1 / distance;
            }
            r.scores[i].v[j] = total / (double)count;
            if (r.scores[i].v[j] > 0)
                positives.v[positives.n++] = r.scores[i].v[j];
        }
    }
    double mean = positives.n ? d_sum(positives) / (double)positives.n : 0;
    for (int i = 0; i < positives.n; i++)
        positives.v[i] = (positives.v[i] - mean) * (positives.v[i] - mean);
    r.threshold = mean + (positives.n > 1 ? sqrt(d_sum(positives) / (double)(positives.n - 1))
                                          : d_sum(positives));
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (r.scores[i].v[j] >= r.threshold)
                r.related[i].v[j] = r.related[j].v[i] = 1;
    ta_release(c, indices.v);
    ta_release(c, means.v);
    ta_release(c, positives.v);
    return r;
}
I reconcile_roles(ta_context *c, I seeds, Recurrence r) {
    if (seeds.n != r.n)
        ta_fail(c, TA_FAILED, "recurrence role dimensions");
    I roles = i_copy(c, seeds);
    B removed = ta_new_bytes(c, seeds.n);
    int n = roles.n, next = 61;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (r.related[i].v[j]) {
                if (roles.v[i] == 50 && roles.v[j] == 40) {
                    roles.v[i] = 0;
                    removed.v[i] = 1;
                } else if (roles.v[i] == 40 && roles.v[j] == 50) {
                    roles.v[j] = 0;
                    removed.v[j] = 1;
                }
            }
    for (int i = 0; i < n; i++) {
        bool created = false;
        for (int j = i + 1; j < n; j++)
            if (r.related[i].v[j]) {
                if (!roles.v[i]) {
                    if (!roles.v[j]) {
                        roles.v[i] = next;
                        roles.v[j] = next;
                        created = true;
                    } else if (roles.v[j] == 40) {
                        if (!removed.v[i])
                            roles.v[i] = 40;
                    } else if (roles.v[j] != 10 && roles.v[j] != 50)
                        roles.v[i] = roles.v[j];
                } else if (!roles.v[j] && roles.v[i] != 10 && roles.v[i] != 50) {
                    if (roles.v[i] != 40 || !removed.v[j])
                        roles.v[j] = roles.v[i];
                }
            }
        if (created)
            next++;
    }
    if (n && roles.v[n - 1] != 40)
        roles.v[n - 1] = 60;
    for (int i = 1; i < n - 1; i++)
        if (!roles.v[i])
            roles.v[i] = next++;
    ta_release(c, removed.v);
    return roles;
}
static int int_order(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}
static double segment(ta_context *c, D a, int start, int end) {
    if (end - start < 2) {
        if (end < 1 || end > a.n)
            ta_fail(c, TA_FAILED, "seed segment range");
        return a.v[end - 1];
    }
    return d_sum(d_view(c, a, start, end)) / (double)(end - start);
}
SeedEvidence non_high_seeds(ta_context *c, D middleLevels, D opening, I boundaries, D vocalActivity,
                            int mood) {
    if (!middleLevels.n || !boundaries.n || !vocalActivity.n)
        ta_fail(c, TA_FAILED, "seed dimensions");
    D envelope = ta_new_doubles(c, ta_max(opening.n, middleLevels.n * 4));
    memcpy(envelope.v, opening.v, opening.n * sizeof(double));
    D middle = d_normalized(c, middleLevels), vocal = d_normalized(c, vocalActivity);
    I relative = ta_new_ints(c, boundaries.n + 1);
    relative.n = 0;
    for (int i = 0; i < boundaries.n; i++) {
        int v = (boundaries.v[i] - boundaries.v[0]) / 4 + 1;
        if (v)
            relative.v[relative.n++] = v;
    }
    qsort(relative.v, relative.n, sizeof(int), int_order);
    int n = relative.n;
    if (!n)
        ta_fail(c, TA_FAILED, "seed relative boundaries");
    SeedEvidence e = {ta_new_ints(c, n), ta_new_doubles(c, n), ta_new_doubles(c, n),
                      ta_new_doubles(c, n)};
    if (d_sum(middle) == 0)
        goto done;
    int nonzero = 0;
    for (int i = 0; i < middle.n; i++)
        if (middle.v[i] != 0)
            nonzero++;
    relative.v[n] = nonzero - 2;
    if (relative.v[n] < relative.v[n - 1])
        ta_fail(c, TA_FAILED, "seed terminal boundary");
    for (int i = 0; i < n; i++) {
        int start = relative.v[i], end = relative.v[i + 1];
        if (start < 1 || end < 2 || end - 2 >= middle.n || end * 4 - 5 >= envelope.n ||
            end * 4 - 5 >= vocal.n)
            ta_fail(c, TA_FAILED, "seed range");
        e.level.v[i] = segment(c, envelope, start * 4 - 4, end * 4 - 4);
        e.vocal.v[i] = segment(c, vocal, start * 4 - 4, end * 4 - 4);
        e.middle.v[i] = segment(c, middle, start - 1, end - 1);
    }
    D *arrays[] = {&e.level, &e.vocal, &e.middle};
    for (int k = 0; k < 3; k++) {
        double m = d_max(*arrays[k]), scale = m == 0 ? 333.3333333333333 : 1 / m;
        for (int i = 0; i < n; i++)
            arrays[k]->v[i] *= scale;
    }
    D active = ta_new_doubles(c, n);
    active.n = 0;
    for (int i = 0; i < n; i++)
        if (e.vocal.v[i] != 0)
            active.v[active.n++] = e.vocal.v[i];
    double vocalMean = active.n ? d_sum(active) / (double)active.n : 0,
           levelMean = d_sum(e.level) / (double)n;
    ta_release(c, active.v);
    e.roles.v[0] = 10;
    for (int i = 0; i < n; i++) {
        bool eligible = e.middle.v[i] >= 0.4 && (vocalMean < 0.4 || e.vocal.v[i] >= 0.4);
        bool strong = (levelMean > 0.8 && e.level.v[i] > 0.92) ||
                      (levelMean <= 0.8 && e.level.v[i] > 0.91) ||
                      (e.level.v[i] > 0.8 && e.vocal.v[i] > 0.88);
        if (!eligible)
            continue;
        if (strong) {
            e.roles.v[i] = 40;
            continue;
        }
        if (!i)
            continue;
        if (e.vocal.v[i] == 0)
            e.vocal.v[i] = 0.003;
        if (e.vocal.v[i - 1] == 0)
            e.vocal.v[i - 1] = 0.003;
        if (e.level.v[i] > 0.85 && e.vocal.v[i] > 0.65 && e.vocal.v[i] / e.vocal.v[i - 1] > 1.3) {
            e.roles.v[i] = 40;
            continue;
        }
        if (e.level.v[i] == 0)
            e.level.v[i] = 0.003;
        if (e.level.v[i - 1] == 0)
            e.level.v[i - 1] = 0.003;
        double ratio = e.level.v[i] / e.level.v[i - 1];
        if ((e.level.v[i] > 0.85 && ratio > 1.5) || (e.roles.v[i - 1] == 40 && ratio > 0.88))
            e.roles.v[i] = 40;
    }
    for (int i = 1; i < n; i++) {
        double decline = mood == MOOD_MID ? (e.roles.v[i - 1] == 40 ? 0.6 : 0.5)
                                          : (e.roles.v[i - 1] == 40 ? 0.62 : 0.54);
        if (e.roles.v[i - 1] == 10 && e.vocal.v[i] == 0) {
            if (e.roles.v[i] != 40)
                e.roles.v[i] = 10;
        } else if (e.roles.v[i] != 40 && e.vocal.v[i] < e.vocal.v[i - 1] * decline)
            e.roles.v[i] = 50;
    }
done:
    ta_release(c, envelope.v);
    ta_release(c, middle.v);
    ta_release(c, vocal.v);
    ta_release(c, relative.v);
    return e;
}
