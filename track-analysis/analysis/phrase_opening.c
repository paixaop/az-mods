/* SPDX-License-Identifier: GPL-3.0-only */
#include "phrase.h"
static double safe(double x) { return x == 0 ? 0.003 : x; }
static double sum(D a, int low, int high) {
    if (low > high || low >= a.n || high < 0)
        return 0;
    int start = ta_max(0, low), end = ta_min(a.n - 1, high);
    return d_sum((D){a.v + start, end - start + 1});
}
static D normalized(ta_context *c, D input, bool guard) {
    double maximum = d_max(input), factor = guard && maximum == 0 ? 333.3333333333333 : 1 / maximum;
    D result = d_copy(c, input);
    for (int i = 0; i < input.n - 1; i++)
        result.v[i] *= factor == 0 ? 0.003 : factor;
    return result;
}
static D ratios(ta_context *c, D values) {
    D out = ta_new_doubles(c, 28);
    for (int i = 1; i < 28; i++)
        out.v[i] = safe(values.v[i]) / safe(values.v[i - 1]);
    return out;
}
static D symmetric(ta_context *c, D ratios) {
    D out = d_copy(c, ratios);
    for (int i = 0; i < out.n; i++)
        if (out.v[i] < 1)
            out.v[i] = 1 / out.v[i];
    return out;
}
static int envelope_candidate(ta_context *c, D envelope, int phase) {
    int n = (envelope.n - phase + 1) / 4;
    D level = ta_new_doubles(c, n);
    for (int i = 0; i < n - 1; i++)
        level.v[i] = sum(envelope, phase - 1 + i * 4, phase + 2 + i * 4) * 0.25;
    double factor = 1 / d_max(level);
    for (int i = 0; i < n - 1; i++)
        level.v[i] *= factor == 0 ? 0.003 : factor;
    double changes[25] = {0};
    I candidates = ta_new_ints(c, 25), accepted = ta_new_ints(c, 100);
    candidates.n = accepted.n = 0;
    for (int i = 1; i < 25; i++) {
        if (level.v[i - 1] == 0)
            level.v[i - 1] = 0.003;
        changes[i] = level.v[i] / level.v[i - 1];
    }
    for (int i = 0; i < 25; i++)
        if (changes[i] >= 2 && level.v[i] > 0.095)
            candidates.v[candidates.n++] = i + 1;
    for (int k = 0; k < candidates.n; k++) {
        int i = candidates.v[k];
        double base = safe(level.v[i - 1]), a = safe(sum(level, i, i + 1) * 0.5) / base,
               b = safe(sum(level, i, i + 2) / 3) / base;
        if (ta_dmax(a, 1 / a) <= 1.2 && ta_dmax(b, 1 / b) <= 1.3)
            accepted.v[accepted.n++] = i;
    }
    double shortDeviation[26] = {0}, longDeviation[26] = {0}, priorDeviation[26] = {0},
           contrast[26] = {0};
    if (accepted.n < candidates.n) {
        double shortValues[3] = {0}, longValues[7] = {0};
        for (int k = 0; k < candidates.n; k++) {
            int i = candidates.v[k];
            double sm = sum(level, i, i + 2) / 3,
                   lm = sum(level, i, ta_min(i + 6, n - 1)) / (double)ta_min(7, ta_max(1, n - i));
            for (int j = 0; j < 7; j++)
                if (i + j + 1 < n) {
                    longValues[j] = fabs(level.v[i + j] - lm);
                    if (j < 3)
                        shortValues[j] = fabs(level.v[i + j] - sm);
                }
            shortDeviation[i] = d_sum((D){shortValues, 3}) / 3;
            longDeviation[i] = d_sum((D){longValues, 7}) / 7;
            if (shortDeviation[i] <= 0.1 && longDeviation[i] <= 0.13 &&
                (!accepted.n || accepted.v[accepted.n - 1] != i))
                accepted.v[accepted.n++] = i;
        }
    }
    if (accepted.n < candidates.n)
        for (int k = 0; k < candidates.n; k++) {
            int i = candidates.v[k], length = ta_min(4, i - 1);
            D prior = d_view(c, level, i - 1 - length, i - 1);
            double mean = safe(d_sum(prior) / (double)prior.n), deviations[4];
            for (int j = 0; j < prior.n; j++)
                deviations[j] = fabs(prior.v[j] - mean);
            priorDeviation[i] = d_sum((D){deviations, prior.n}) / (double)prior.n;
            contrast[i] = level.v[i - 1] / mean;
            if (shortDeviation[i] <= 0.14 && longDeviation[i] <= 0.16 &&
                priorDeviation[i] <= 0.04 && contrast[i] >= 3 &&
                (!accepted.n || accepted.v[accepted.n - 1] != i))
                accepted.v[accepted.n++] = i;
        }
    if (accepted.n < candidates.n)
        for (int k = 0; k < candidates.n; k++) {
            int i = candidates.v[k];
            if (shortDeviation[i] <= 0.18 && longDeviation[i] <= 0.3 && priorDeviation[i] <= 0.01 &&
                contrast[i] >= 3 && (!accepted.n || accepted.v[accepted.n - 1] != i))
                accepted.v[accepted.n++] = i;
        }
    I unique = i_compact(c, accepted, true);
    int result = !unique.n                                        ? 0
                 : unique.n > 1 && unique.v[1] == unique.v[0] + 1 ? unique.v[1]
                                                                  : unique.v[0];
    ta_release(c, level.v);
    ta_release(c, candidates.v);
    ta_release(c, accepted.v);
    ta_release(c, unique.v);
    return result;
}
static int positive(D a, int start, int end) {
    int count = 0;
    for (int i = start; i < end; i++)
        if (a.v[i] > 0)
            count++;
    return count;
}
static int bar(int beat) { return (int)round((double)(beat - 1) * 0.25 + 1); }
static int vocal_candidate(ta_context *c, D vocal) {
    double maximum = d_max(vocal), scale = maximum == 0 ? 333.3333333333333 : 1 / maximum;
    double values[112];
    D v = {values, 112};
    for (int i = 0; i < 112; i++)
        values[i] = vocal.v[i] * scale;
    int dense[96], countDense = 0;
    for (int i = 0; i < 96; i++)
        if (v.v[i] >= 0.5 && (!(i % 4) || v.v[i + 1] + v.v[i + 2] + v.v[i + 3] != 0)) {
            int count = positive(v, i, i + 8);
            double mean = sum(v, i, i + 7) / (double)count;
            if ((count >= 5 && mean >= 0.4) || (count == 4 && mean >= 0.5))
                dense[countDense++] = i + 1;
        }
    int onset = 0;
    for (int i = 5; i < 97; i++) {
        if (i > 34 && sum(v, 0, i - 1) != 0)
            break;
        if (!(v.v[i] > 0))
            continue;
        int count = positive(v, i, i + 16);
        double mean = sum(v, i, i + 15) / 16;
        if (((count > 7 && mean > 0.1) || (count > 5 && mean > 0.2)) &&
            (v.v[i] * v.v[i + 2] * v.v[i + 3] > 0 || v.v[i] * v.v[i + 1] > 0) &&
            sum(v, 4, i - 1) / (double)(i - 4) < 0.05) {
            onset = i + 1;
            break;
        }
    }
    if (!onset) {
        for (int i = 8; i < 34; i++) {
            bool adjacent = false;
            int previous = 0;
            for (int j = 0; j < i / 4; j++) {
                int count = positive(v, j * 4, j * 4 + 4);
                if (j && previous >= 3 && count >= 3)
                    adjacent = true;
                previous = count;
            }
            if (adjacent)
                break;
            if (!(v.v[i] > 0))
                continue;
            bool denseNext = true;
            for (int j = 0; j < 3; j++)
                if (positive(v, i + j * 4, i + j * 4 + 4) <= 2)
                    denseNext = false;
            double mean = sum(v, i, i + 11) / 12;
            if (denseNext && mean > 0.4 && sum(v, 0, i - 1) / (double)i < mean * 0.5) {
                onset = i + 1;
                break;
            }
        }
        if (!onset)
            for (int i = 7; i < 66; i++)
                if (v.v[i] > 0) {
                    int count = positive(v, i, i + 8);
                    if (count > 2 && sum(v, i, i + 23) / 24 >= 0.1 &&
                        sum(v, 0, i - 1) / (double)i < 0.04 &&
                        (sum(v, i, i + 7) / (double)count >= 0.65 ||
                         (v.v[i] > 0.7 && v.v[i + 1] > 0.3 && v.v[i + 2] > 0.3))) {
                        onset = i + 1;
                        break;
                    }
                }
    }
    int result = bar(onset), first = countDense ? dense[0] : 0, denseBar = bar(first);
    if ((result == 8 && denseBar == 9) || (result == 4 && denseBar == 5) ||
        (!(denseBar == 1 || (result == 5 && denseBar == 4) || (result == 9 && denseBar == 8)) &&
         !(result < denseBar && result != 1))) {
        result = denseBar;
        onset = first;
    }
    if (result > 1) {
        if (onset >= 14 && onset <= 19)
            result = 5;
        if (onset >= 30 && onset <= 35)
            result = 9;
        if (!(onset >= 14 && onset <= 19) && !(onset >= 30 && onset <= 35) &&
            (result == 4 || result == 8) && countDense > 1 && bar(dense[1]) == result + 1)
            result++;
    }
    (void)c;
    return result;
}
static int level_candidate(ta_context *ctx, D values, D ratios, D sym, bool low) {
    I ranked = rank_d(ctx, (D){sym.v, 17}, true);
    int n = 0;
    for (int i = 0; i < 17; i++) {
        int value = ranked.v[i] + 1;
        if (i == 16 || value > 3 || sym.v[value - 1] >= 1.35)
            ranked.v[n++] = value;
    }
    ranked.n = n;
    I candidates = i_copy(ctx, (I){ranked.v, ta_min(n, 5)});
    if (candidates.v[1] - candidates.v[0] == 1)
        i_remove(&candidates, 1);
    else if (candidates.v[0] - candidates.v[1] == 1)
        i_remove(&candidates, 0);
    if (candidates.v[2] - candidates.v[1] == 1)
        i_remove(&candidates, 2);
    else if (candidates.v[1] - candidates.v[0] == 1)
        i_remove(&candidates, 1);
    candidates.n = ta_min(candidates.n, 3);
    I ordered = i_sorted(ctx, candidates, false);
    ta_release(ctx, candidates.v);
    candidates = ordered;
    if (!low) {
        int a = candidates.v[0], b = candidates.v[1], c = candidates.v[2];
        bool first =
            !(a > 2 && b - a == 2 &&
              (safe(sym.v[b - 1]) / safe(sym.v[a - 1]) > 2 ||
               sym.v[b] + sym.v[b - 2] + sym.v[b + 1] < sym.v[a] + sym.v[a - 2] + sym.v[a + 1]));
        bool second = !(c - b == 2 && (sym.v[c - 1] / safe(sym.v[b - 1]) > 2 ||
                                       sym.v[b] + sym.v[b - 2] + sym.v[b + 1] >
                                           sym.v[c] + sym.v[c - 2] + sym.v[c + 1]));
        if (!first && !second) {
            candidates.v[0] = c;
            candidates.v[2] = a;
        } else if (!first) {
            candidates.v[0] = b;
            candidates.v[1] = a;
        } else if (!second) {
            candidates.v[1] = c;
            candidates.v[2] = b;
        }
    }
    int result = 0;
    for (int k = 0; k < candidates.n; k++) {
        int candidate = candidates.v[k];
        bool accepted = false;
        if (candidate == 2)
            accepted = ratios.v[1] > 1 && (sym.v[2] + sym.v[3]) * 0.5 < 1.4 && sym.v[1] > 1.5;
        else if (candidate > 2) {
            int start = ratios.v[1] >= 0.85 ? 0 : 1;
            D preceding = d_view(ctx, values, start, candidate - 1);
            double mean = d_sum(preceding) / (double)preceding.n, dev[28];
            for (int i = 0; i < preceding.n; i++)
                dev[i] = fabs(mean - preceding.v[i]);
            double deviation = d_sum((D){dev, preceding.n}) / (double)preceding.n;
            if (low)
                deviation = safe(deviation);
            double current = values.v[candidate - 1];
            bool changed = deviation >= 0.005
                               ? current / (mean + deviation) > 1.15 ||
                                     current / (mean - deviation) < 0.8695652173913044
                               : fabs(mean - current) / deviation > 15;
            if (!changed && candidate > 3 && sym.v[candidate - 1] > 1.3) {
                int count = candidate < 10 ? 2 : 4;
                D prior = d_view(ctx, values, candidate - 1 - count, candidate - 1);
                double m = d_sum(prior) / (double)count;
                for (int i = 0; i < count; i++)
                    dev[i] = fabs(m - prior.v[i]);
                double d = d_sum((D){dev, count}) / (double)count;
                changed = current / (m + d) > 1.2 || current / (m - d) < 0.8333333333333334;
            }
            if (changed && candidate + 3 <= 27) {
                if (values.v[candidate + 2] == 0)
                    values.v[candidate + 2] = 0.003;
                double a = (sym.v[candidate] + sym.v[candidate + 1]) * 0.5,
                       b = (sym.v[candidate + 1] + sym.v[candidate + 2]) * 0.5,
                       ratio = values.v[candidate] / values.v[candidate + 2];
                accepted = a < 1.4 || b < 1.4 || a < sym.v[candidate - 1] * 0.6 ||
                           b < sym.v[candidate - 1] * 0.6 || ta_dmax(ratio, 1 / ratio) < 1.1;
                if (!accepted)
                    continue;
            }
            if (!accepted && candidate > 3) {
                int count = low ? 3 : 2;
                double m = safe(sum(values, candidate - 1 - count, candidate - 2) / (double)count),
                       ratio = current / m;
                accepted = !(ratio <= 1.4 && ratio >= 0.7);
            }
        }
        if (accepted) {
            if (values.v[candidate + 1] == 0)
                values.v[candidate + 1] = 0.003;
            result = candidate;
            if (candidate < 3 && ratios.v[candidate - 1] <= 1.6)
                break;
            if (ratios.v[candidate - 1] <= 1)
                result += ratios.v[candidate] * ratios.v[candidate - 1] > 0.83;
            else if (ratios.v[candidate] > 1.15 || ratios.v[candidate] < 0.6) {
                double ratio = values.v[candidate - 1] / values.v[candidate + 1];
                result += ratio > 1.05 || ratio < 0.95;
            }
            break;
        }
    }
    ta_release(ctx, ranked.v);
    ta_release(ctx, candidates.v);
    return result;
}
static Opening reconcile(ta_context *c, I raw, D low, D lowRatios, D lowSym, int phase) {
    Opening out = {
        .hypotheses = i_copy(c, raw), .raw = raw, .scores = ta_new_doubles(c, 4), .found = true};
    I h = out.hypotheses;
    D scores = out.scores;
    int selected = 0;
    for (int i = 0; i < 4; i++)
        if (h.v[i] == 1)
            h.v[i] = 0;
    for (int i = 0; i <= 1; i++)
        if (h.v[i]) {
            int count = 0;
            for (int j = 0; j < 4; j++)
                if (h.v[j] == h.v[i])
                    count++;
            if (count >= 3) {
                selected = h.v[i];
                break;
            }
        }
    if (!selected) {
        for (int i = 0; i < 4; i++)
            if (h.v[i]) {
                int count = 0;
                for (int j = 0; j < 4; j++)
                    if (h.v[j] == h.v[i])
                        count++;
                if (count == 2)
                    scores.v[i] += 1;
            }
        I unique = i_compact(c, h, true);
        for (int i = 0; i < 4; i++)
            if (h.v[i]) {
                int rank = 0;
                while (rank < unique.n && unique.v[rank] != h.v[i])
                    rank++;
                if (!rank)
                    scores.v[i] += unique.n > 1 && unique.v[1] - unique.v[0] > 3 ? 0.7 : 0.4;
                else if (rank == 1) {
                    if (unique.v[1] - unique.v[0] < 4)
                        scores.v[i] += 0.2;
                } else
                    scores.v[i] -= 0.35;
            }
        double values[4];
        for (int i = 0; i < 4; i++)
            values[i] = h.v[i];
        I order = rank_d(c, (D){values, 4}, true);
        for (int i = 0; i < 3; i++)
            if (h.v[order.v[i]] - h.v[order.v[i + 1]] == 1) {
                scores.v[order.v[i]] += 0.75;
                if (order.v[i + 1] == 0)
                    scores.v[0] += 0.86;
                else if (order.v[i] == 2 && order.v[i + 1] == 3)
                    scores.v[2] += 0.3;
            }
        for (int i = 0; i < 4; i++) {
            int v = h.v[i];
            if (!v)
                scores.v[i] = 0;
            else
                scores.v[i] += v == 2              ? 1.2
                               : v == 3            ? 1.3
                               : v == 5 || v == 17 ? 1.8
                               : v == 9            ? 1.85
                               : v >= 18           ? 0.44999999999999996
                                                   : 1;
        }
        ta_release(c, unique.v);
        ta_release(c, order.v);
    }
    int third = h.v[2];
    if (third > 0 && !(lowSym.v[third - 1] >= 1.3) &&
        !((third == 1 ? 0 : fabs(low.v[third - 1] - low.v[third - 2])) >= 0.09)) {
        h.v[2] = 0;
        scores.v[2] = 0;
    }
    if (!selected) {
        I order = rank_d(c, scores, true);
        selected = h.v[order.v[0]];
        int unchanged = 0;
        for (int i = 0; i < 4; i++)
            if (scores.v[i] == scores.v[order.v[i]])
                unchanged++;
        if (raw.v[2] > 0 && unchanged == 1 && scores.v[order.v[1]] == scores.v[2] &&
            abs(selected - h.v[2]) < 5 && scores.v[order.v[0]] - scores.v[2] <= 1.1 &&
            order.v[0] != 0) {
            double value = lowRatios.v[raw.v[2] - 1];
            if (!(value >= 0.32 && value <= 1.5))
                selected = h.v[2];
        }
        ta_release(c, order.v);
    }
    for (int i = 0; i < 4; i++)
        h.v[i] = h.v[i] ? phase - 4 + h.v[i] * 4 : 0;
    out.selected = selected ? phase - 4 + selected * 4 : phase;
    return out;
}
Opening opening_judge(ta_context *c, D envelope, D low, D middle, D vocal, int phase) {
    if (phase < 1 || phase > 4 || envelope.n < 112 || low.n < 28 || middle.n < 28 || vocal.n < 112)
        ta_fail(c, TA_FAILED, "opening dimensions");
    I raw = ta_new_ints(c, 4);
    raw.v[0] = envelope_candidate(c, envelope, phase);
    raw.v[1] = vocal_candidate(c, vocal);
    D lv = normalized(c, low, false), lr = ratios(c, lv), ls = symmetric(c, lr);
    ls.v[0] = 0;
    for (int i = 1; i <= 16; i++)
        if (lv.v[i - 1] < 0.02 && lv.v[i] < 0.02 && ls.v[i] < 3)
            ls.v[i] = 1;
    raw.v[2] = level_candidate(c, lv, lr, ls, true);
    D mv = normalized(c, middle, true), mr = ratios(c, mv), ms = symmetric(c, mr);
    ms.v[0] = 0;
    raw.v[3] = level_candidate(c, mv, mr, ms, false);
    D original = normalized(c, low, false), originalRatios = ratios(c, original),
      originalSym = symmetric(c, originalRatios);
    Opening out = reconcile(c, raw, original, lr, originalSym, phase);
    D arrays[] = {lv, lr, ls, mv, mr, ms, original, originalRatios, originalSym};
    for (size_t i = 0; i < sizeof arrays / sizeof *arrays; i++)
        ta_release(c, arrays[i].v);
    return out;
}
static D repeat(ta_context *c, D values, int count, int times) {
    D out = ta_new_doubles(c, count * times);
    for (int i = 0; i < times; i++)
        memcpy(out.v + i * count, values.v, count * sizeof(double));
    return out;
}
Opening opening_main(ta_context *c, D envelope, D low, D middle, D vocal, int phase, I anchors) {
    if (!anchors.n || phase < 1 || phase > 4)
        ta_fail(c, TA_FAILED, "opening anchors");
    int n = ta_min(ta_min(envelope.n / 4, low.n), ta_min(middle.n, vocal.n / 4));
    Opening out = {
        .hypotheses = ta_new_ints(c, 1), .raw = ta_new_ints(c, 0), .scores = ta_new_doubles(c, 0)};
    if (n >= 8) {
        D e = envelope, l = low, m = middle, v = vocal;
        if (n < 28) {
            int times = (n + 27) / n + 2;
            e = repeat(c, envelope, n * 4, times);
            l = repeat(c, low, n, times);
            m = repeat(c, middle, n, times);
            v = repeat(c, vocal, n * 4, times);
        }
        ta_release(c, out.hypotheses.v);
        ta_release(c, out.raw.v);
        ta_release(c, out.scores.v);
        out = opening_judge(c, e, l, m, v, phase);
        if (n < 28) {
            ta_release(c, e.v);
            ta_release(c, l.v);
            ta_release(c, m.v);
            ta_release(c, v.v);
        }
        int sum = 0;
        for (int i = 0; i < out.hypotheses.n; i++)
            sum += out.hypotheses.v[i];
        if (!sum) {
            if (n >= 28) {
                out.found = true;
                out.hypotheses.n = 1;
                out.hypotheses.v[0] = phase;
                out.selected = phase;
            } else
                out.found = false;
        } else if (n < 28) {
            out.found = true;
            int retained = 0;
            for (int i = 0; i < out.hypotheses.n; i++)
                if (out.hypotheses.v[i] <= n) {
                    retained += out.hypotheses.v[i];
                    out.selected = out.hypotheses.v[i];
                }
            if (!retained) {
                out.hypotheses.n = 1;
                out.hypotheses.v[0] = phase;
                out.selected = phase;
            }
        }
    }
    I ordered = i_sorted(c, out.hypotheses, true);
    ta_release(c, out.hypotheses.v);
    out.hypotheses = ordered;
    if (anchors.v[0]) {
        bool matched = false;
        for (int i = anchors.n - 1; i >= 0; i--)
            for (int j = 0; j < ordered.n; j++)
                if (anchors.v[i] == ordered.v[j]) {
                    out.selected = anchors.v[i];
                    out.found = true;
                    matched = true;
                }
        if (!matched) {
            int remainder = ((anchors.v[0] - phase) / 4 + 1) % 4, same = 0;
            for (int i = 0; i < ordered.n - 1; i++) {
                int difference = ordered.v[i] - phase,
                    bar = difference / 4 + (difference > 3 ? 1 : 0);
                if (bar % 4 == remainder)
                    same = same ? ta_min(same, ordered.v[i]) : ordered.v[i];
            }
            if (same) {
                out.selected = same;
                out.found = true;
            }
        }
    }
    return out;
}
