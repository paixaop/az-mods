/* SPDX-License-Identifier: GPL-3.0-only */
#include "phrase.h"
bool beat_four(ta_context *c, int start, int end, bool last, D low, D quarters) {
    if (start < 1 || end <= start || end - 2 >= low.n)
        return false;
    double mean = d_sum(d_view(c, low, start - 1, end - 1)) / (end - start);
    int qc = (end - start) * 4 - 1;
    if (qc > 48 && !last)
        qc -= 32;
    int n = qc / 4;
    if (n <= 0 || (start - 1 + n) * 4 > quarters.n)
        return false;
    I positions = ta_new_ints(c, n);
    D amplitudes = ta_new_doubles(c, n), ratios = ta_new_doubles(c, n);
    for (int i = 1; i < n; i++) {
        int base = (start - 1 + i) * 4, peak = 0;
        for (int j = 1; j < 4; j++)
            if (quarters.v[base + j] > quarters.v[base + peak])
                peak = j;
        positions.v[i] = peak + 1;
        amplitudes.v[i] = quarters.v[base + peak];
        int distance = abs(positions.v[i] - positions.v[i - 1]);
        if (distance == 0)
            ratios.v[i - 1] = 1;
        else if (distance == 1 || distance == 3)
            ratios.v[i - 1] = quarters.v[base - 1 + positions.v[i - 1]] /
                              (amplitudes.v[i] == 0 ? 0.003 : amplitudes.v[i]);
    }
    bool accepted = d_sum(ratios) / n > 0.8;
    int stable = ta_max(0, n - 4);
    if (!accepted && stable > 2) {
        double total = 0;
        for (int i = 2; i < stable; i++) {
            if (amplitudes.v[i] == 0)
                amplitudes.v[i] = 0.003;
            if (amplitudes.v[i - 1] == 0)
                amplitudes.v[i - 1] = 0.003;
            total += ta_dmax(amplitudes.v[i] / amplitudes.v[i - 1],
                             amplitudes.v[i - 1] / amplitudes.v[i]);
        }
        accepted = total / (stable - 2) < 1.12;
    }
    ta_release(c, positions.v);
    ta_release(c, amplitudes.v);
    ta_release(c, ratios.v);
    return accepted && mean >= 0.22;
}
UpAnalysis simple_loops(ta_context *c, I b, I roles, D first, D second, D metric) {
    int n = roles.n;
    if (b.n != n || metric.n != n)
        ta_fail(c, TA_FAILED, "loop dimensions");
    UpAnalysis r = {i_copy(c, roles), ta_new_ints(c, n), ta_new_doubles(c, n)};
    for (int i = 0; i < n; i++)
        if (roles.v[i] == 30 || roles.v[i] == 50) {
            int start = b.v[i], end = i + 1 < n ? b.v[i + 1] : first.n + 1, length = end - start;
            if (start <= 0 || end - 2 >= ta_min(first.n, second.n))
                continue;
            double maximum = 0, minimum = 0, fd = 0, sd = 0, slope = 0;
            int maxAt = end - 8, minAt = start;
            if (length >= 16) {
                int begin = length >= 32 ? start + length / 2 : start;
                for (int p = begin; p <= end - 8; p++) {
                    double v = d_sum(d_view(c, second, p - 1, p + 7)) * 0.125;
                    if (v > maximum) {
                        maximum = v;
                        maxAt = p;
                    }
                }
                minimum = maximum;
                int last = length >= 32 ? start + length / 2 - 2 : end - 8;
                for (int p = start; p <= last; p++) {
                    double v = d_sum(d_view(c, second, p - 1, p + 7)) * 0.125;
                    if (v < minimum) {
                        minimum = v;
                        minAt = p;
                    }
                }
                r.metrics.v[i] = maximum;
                fd = (d_sum(d_view(c, first, maxAt - 1, maxAt + 7)) -
                      d_sum(d_view(c, first, minAt - 1, minAt + 7))) *
                     0.125;
                sd = maximum - minimum;
                if (maxAt != minAt)
                    slope = sd / (maxAt - minAt);
            }
            if (metric.v[i] >= 0.16)
                continue;
            bool primary = fd < 10 && sd > 85 && slope > 3.5 && slope < 22,
                 alternate = sd - fd > 150 && sd > 85 && slope < 22,
                 secondary = sd > 85 && slope > 6 && slope < 22;
            if (primary || alternate || secondary) {
                r.roles.v[i] = 31;
                r.starts.v[i] = start;
            }
        }
    return r;
}
I restore_loops(ta_context *c, I roles, I saved, D metric) {
    int n = roles.n;
    if (saved.n != n || metric.n != n)
        ta_fail(c, TA_FAILED, "restore loop dimensions");
    I r = i_copy(c, roles), spans = ta_new_ints(c, n);
    int remaining = 0, runEnd = n;
    for (int i = 0; i < n - 1; i++)
        if (roles.v[i] == 31) {
            for (int next = i + 1; next < n; next++)
                if (roles.v[next] != 31) {
                    runEnd = next + 1;
                    break;
                }
            if (remaining == 0) {
                remaining = runEnd - (i + 1);
                spans.v[i] = remaining;
            }
            remaining--;
        }
    for (int i = 0; i < n - 1; i++)
        if (spans.v[i] > 1) {
            int end = i + spans.v[i], selected = i;
            for (int j = i + 1; j < end; j++)
                if (metric.v[j] > metric.v[selected])
                    selected = j;
            for (int j = i; j < end; j++)
                if (j != selected)
                    r.v[j] = saved.v[j];
        }
    ta_release(c, spans.v);
    return r;
}
static double average(I counts, int from, int to, double scale) {
    double total = 0;
    for (int i = counts.n + from; i <= counts.n + to; i++)
        total += counts.v[i];
    return total * scale;
}
RhythmAnalysis decorate_rhythm(ta_context *c, I b, I roles, B activity) {
    int n = roles.n;
    if (b.n != n)
        ta_fail(c, TA_FAILED, "rhythm decorator dimensions");
    RhythmAnalysis r = {i_copy(c, roles), ta_new_ints(c, n), ta_new_ints(c, n), ta_new_ints(c, n),
                        ta_new_ints(c, n)};
    if (n <= 1)
        return r;
    int firstEight = 0, afterEights = 0, precedingFour = 0;
    for (int i = 0; i < n - 1; i++) {
        int baseRole = roles.v[i] % 100;
        if (!(baseRole == 10 || baseRole == 11 || baseRole == 30 || baseRole == 31 ||
              baseRole == 50 || baseRole == 51))
            continue;
        int start = b.v[i], length = b.v[i + 1] - start, groups = length / 4;
        if (groups <= 0 || start <= 0 || (start - 1) * 4 + groups * 16 > activity.n)
            continue;
        I codes = ta_new_ints(c, groups + 8);
        for (int g = 0; g < groups; g++) {
            int base = (start - 1) * 4 + g * 16;
            bool four = true, eight = true;
            for (int j = 0; j < 16; j++) {
                four &= activity.v[base + j] == (j % 4 == 0);
                eight &= activity.v[base + j] == (j % 2 == 0);
            }
            if (four)
                codes.v[g] = 4;
            if (eight)
                codes.v[g] = 8;
            else if (g > 1 && codes.v[g - 1] == 8) {
                bool all = true;
                for (int j = codes.v[g - 2] == 8 ? 2 : 0; j < 4; j++)
                    all &= activity.v[base + j] == 1;
                if (all)
                    codes.v[g] = 16;
            }
        }
        bool found = false, tail = false, preceding = false;
        for (int g = 0; g < groups; g++)
            if (codes.v[g] == 8) {
                if (found)
                    break;
                firstEight = g + 1;
                found = true;
                if (firstEight < groups) {
                    if (codes.v[g + 1] == 8) {
                        if (codes.v[g + 2] == 8) {
                            if (g + 5 < groups) {
                                afterEights = g + 5;
                                tail = true;
                            }
                        } else if (g + 3 < groups) {
                            afterEights = g + 3;
                            tail = true;
                        }
                    } else {
                        preceding = firstEight > 1;
                        if (firstEight >= 2)
                            precedingFour = g;
                        tail = true;
                        afterEights = g + 2;
                    }
                }
            }
        if (tail && afterEights <= groups && afterEights > 0 && codes.v[afterEights - 1] == 16) {
            int count = afterEights - firstEight,
                offset = count == 1  ? 8
                         : count < 3 ? 16
                                     : 32,
                second = start + firstEight * 4 - 4, proposed = second - offset;
            r.roles.v[i] = 32;
            r.starts.v[i] = proposed >= start ? proposed : second - 4;
            r.first.v[i] = r.starts.v[i];
            r.second.v[i] = second;
            r.third.v[i] = start + (afterEights - 1) * 4;
        }
        if (r.roles.v[i] != 32 && preceding && precedingFour > 0 && length - 1 > 34 &&
            codes.v[precedingFour - 1] == 4 && precedingFour == groups - 2) {
            r.roles.v[i] = 32;
            r.starts.v[i] = start + (precedingFour - 1) * 4;
            r.first.v[i] = r.starts.v[i];
            r.second.v[i] = r.starts.v[i] + 4;
            r.third.v[i] = r.starts.v[i] + 8;
        }
        ta_release(c, codes.v);
    }
    for (int hook = 1; hook < n; hook++)
        if (roles.v[hook] == 40) {
            int i = hook - 1, base = roles.v[i] % 100, length = b.v[hook] - b.v[i],
                groups = length / 4;
            if (r.roles.v[i] == 32 ||
                !(base == 30 || base == 31 || base == 50 || base == 51 ||
                  (length > 35 && (base == 10 || base == 11))) ||
                groups <= 0 || b.v[i] <= 0 || (b.v[i] - 1) * 4 + groups * 16 > activity.n)
                continue;
            I counts = ta_new_ints(c, groups);
            for (int g = 0; g < groups; g++) {
                int p = (b.v[i] - 1) * 4 + g * 16;
                for (int j = 0; j < 16; j++)
                    counts.v[g] += activity.v[p + j];
            }
            double factor = base == 10 || base == 11 ? 0.85 : 0.9;
            bool staged = false, simple = false;
            int extent = 0, secondOffset = 0, thirdOffset = 0;
            if (length >= 32 && length < 64) {
                double a = average(counts, -8, -5, 0.25), b = average(counts, -4, -3, 0.5),
                       d = counts.v[groups - 2];
                staged = a <= b * factor && b <= d * factor;
                simple = !staged && a < (b + d) * 0.5;
                extent = 32;
                secondOffset = 16;
                thirdOffset = 8;
            } else if (length >= 64) {
                double a = average(counts, -16, -9, 0.125), b = average(counts, -8, -5, 0.25),
                       d = average(counts, -4, -2, 0.33333);
                staged = a <= b * factor && b <= d * factor;
                int min = counts.v[0], max = min;
                for (int j = 1; j < groups - 1; j++) {
                    min = ta_min(min, counts.v[j]);
                    max = ta_max(max, counts.v[j]);
                }
                simple = !staged && (max - min < 5 ? b <= d * factor : a < (d + b) * 0.5);
                extent = staged ? 48 : 32;
                secondOffset = 32;
                thirdOffset = 16;
            } else if (length >= 16) {
                double a = average(counts, -4, -3, 0.5), b = counts.v[groups - 2];
                staged = a <= b * factor;
                simple = !staged && a < b;
                extent = 16;
                secondOffset = 16;
                thirdOffset = 8;
            }
            if (staged || simple) {
                r.roles.v[i] = staged ? 32 : 33;
                r.starts.v[i] = b.v[hook] - extent;
                r.first.v[i] = staged ? r.starts.v[i] : 0;
                r.second.v[i] = staged ? b.v[hook] - secondOffset : 0;
                r.third.v[i] = staged ? b.v[hook] - thirdOffset : 0;
            }
            ta_release(c, counts.v);
        }
    for (int i = 0; i < n - 1; i++)
        if ((roles.v[i] % 100 == 10 || roles.v[i] % 100 == 11) &&
            (r.roles.v[i] == 32 || r.roles.v[i] == 33))
            r.roles.v[i] = roles.v[i];
    return r;
}
Outro phrase_outro(ta_context *c, I b, I roles, D low, B activity, D quarters, int phase) {
    if (b.n != roles.n)
        ta_fail(c, TA_FAILED, "outro dimensions");
    Outro out = {i_copy(c, roles), low.n};
    if (!b.n)
        return out;
    int lastStart = b.v[b.n - 1], selected = 0;
    low = d_copy(c, low);
    if (roles.v[roles.n - 1] == 60) {
        int count = activity.n / 4 - lastStart + 1;
        if (count > 0) {
            int lastActive = count;
            for (int i = count - 1; i >= 0; i--) {
                int base = (lastStart - 1 + i) * 4;
                bool active = false;
                for (int j = 0; j < 4; j++)
                    active |= activity.v[base + j] != 0;
                if (active) {
                    lastActive = i + 1;
                    break;
                }
            }
            if (lastActive > count - 13 && lastActive != count)
                selected = lastActive + lastStart;
        }
    }
    if (selected && selected >= 5 && selected - 1 < low.n) {
        if (low.v[selected - 1] == 0)
            low.v[selected - 1] = 0.003;
        if (low.v[selected - 2] == 0)
            low.v[selected - 2] = 0.003;
        if (low.v[selected - 1] / low.v[selected - 2] > 0.8 ||
            d_sum(d_view(c, low, selected - 5, selected - 1)) * 0.25 < 0.05)
            selected = 0;
    }
    int start = lastStart + 9, lastAudible = low.n;
    for (int i = low.n - 1; i >= start - 1; i--)
        if (low.v[i] > 0.0007) {
            lastAudible = i + 2;
            break;
        }
    int count = ta_max(0, lastAudible - start);
    D samples =
        start > 0 && start - 1 < low.n ? d_copy(c, d_view(c, low, start - 1, low.n)) : (D){0};
    D ratios = ta_new_doubles(c, count), accelerations = ta_new_doubles(c, count);
    if (count > 1 && samples.n >= count) {
        for (int i = 1; i < count; i++) {
            if (samples.v[i] == 0)
                samples.v[i] = 0.003;
            if (samples.v[i - 1] == 0)
                samples.v[i - 1] = 0.003;
            ratios.v[i] = samples.v[i] / samples.v[i - 1];
            if (i > 1)
                accelerations.v[i] = ratios.v[i] / ratios.v[i - 1];
        }
        D next = ta_new_doubles(c, count);
        for (int i = 1; i < count; i++)
            if (ratios.v[i] < 0.5) {
                next.n = 0;
                bool rising = false;
                for (int j = i + 1; j < count; j++)
                    if (samples.v[j] > 0.001) {
                        next.v[next.n++] = ratios.v[j];
                        rising |= ratios.v[j] > 1.1;
                    }
                if ((next.n && d_sum(next) / next.n > 0.86) || rising)
                    continue;
                int candidate = start + i;
                if (!selected || candidate < selected) {
                    selected = candidate;
                    if (candidate < lastAudible - 3) {
                        int remainder = (candidate + 1 - phase) % 4;
                        if (remainder == 2 && i > 1 &&
                            (ratios.v[i - 1] > 1.3 || ratios.v[i - 2] > 1.3))
                            selected -= 2;
                        else if (remainder == 3 && i > 2 && ratios.v[i - 2] > 1.3 &&
                                 ratios.v[i - 3] > 1.3)
                            selected -= 3;
                    }
                }
            }
        for (int i = 1; i < count; i++)
            if (samples.v[i] > 0.2 && ratios.v[i] > 2) {
                int j = i + 1;
                while (j < count && samples.v[j] >= 0.0007 && ratios.v[j] <= 1)
                    j++;
                if (j == count || samples.v[j] < 0.0007) {
                    int candidate = start + i;
                    if (!selected || candidate < selected)
                        selected = candidate;
                    break;
                }
            }
        ta_release(c, next.v);
    }
    for (int i = 1; i < count - 2; i++)
        if (accelerations.v[i] > 1.3 && accelerations.v[i + 1] < 1.03 &&
            accelerations.v[i + 2] < 1.03) {
            if (count <= i + 4)
                break;
            bool all = true;
            for (int j = i + 3; j < count; j++)
                all &= accelerations.v[j] <= 0.91;
            if (all) {
                int candidate = start + i;
                if (!selected || candidate < selected)
                    selected = candidate;
                break;
            }
        }
    int terminal = !selected ? lastAudible : selected;
    if (((terminal - phase + 1) & 3) == 0 && terminal < lastAudible)
        terminal++;
    if (beat_four(c, lastStart, terminal, true, low, quarters))
        out.roles.v[out.roles.n - 1]++;
    out.terminal = terminal;
    ta_release(c, low.v);
    ta_release(c, samples.v);
    ta_release(c, ratios.v);
    ta_release(c, accelerations.v);
    return out;
}
Decorated decorate_high(ta_context *c, I b, I roles, FillProfile p, D first, D second, D metric,
                        B activity) {
    if (b.n != roles.n || metric.n != roles.n)
        ta_fail(c, TA_FAILED, "high decorator dimensions");
    int terminal = p.low.n + 1;
    I ends = i_join(c, b, (I){&terminal, 1});
    Fills r = attach_fills(c, ends, roles, p, FILL_INTRO);
    for (int i = 0; i < roles.n - 1; i++)
        if ((r.roles.v[i] == 10 || r.roles.v[i] == 110) &&
            beat_four(c, b.v[i], b.v[i + 1], i == roles.n - 2, p.low, p.quarters))
            r.roles.v[i]++;
    I saved = i_copy(c, r.roles);
    UpAnalysis loop = simple_loops(c, b, r.roles, first, second, metric);
    RhythmAnalysis pattern = decorate_rhythm(c, b, loop.roles, activity);
    r.roles = restore_loops(c, pattern.roles, saved, loop.metrics);
    Fills candidates = attach_fills(c, ends, r.roles, p, FILL_UP_HOOK);
    r.roles = candidates.roles;
    for (int i = 0; i < r.roles.n; i++)
        if (candidates.starts.v[i]) {
            r.starts.v[i] = candidates.starts.v[i];
            r.routes.v[i] = candidates.routes.v[i];
        }
    for (int i = 1; i < roles.n - 1; i++)
        if ((r.roles.v[i] == 40 || r.roles.v[i] == 440) &&
            beat_four(c, b.v[i], b.v[i + 1], i == roles.n - 2, p.low, p.quarters))
            r.roles.v[i]++;
    Outro ending = phrase_outro(c, b, r.roles, p.low, activity, p.quarters, p.phase);
    r.roles = ending.roles;
    ends.v[ends.n - 1] = ending.terminal;
    clean_activity(c, ends, p, &r);
    final_adjust(c, ends, true, metric, &r);
    return (Decorated){ends, r, pattern.starts, pattern.first, pattern.second, pattern.third};
}
