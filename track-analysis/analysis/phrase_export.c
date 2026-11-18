/* SPDX-License-Identifier: GPL-3.0-only */
#include "phrase.h"
static double sum(D a, int start, int end) {
    if ((unsigned)start > (unsigned)end || (unsigned)end >= (unsigned)a.n)
        return 0;
    return d_sequential((D){a.v + start, end - start + 1});
}
Variation phrase_variation(ta_context *c, D envelope, I boundaries) {
    if (boundaries.n < 3)
        return (Variation){0};
    I limits = i_copy(c, boundaries);
    limits.v[limits.n - 1] = ta_min(limits.v[limits.n - 1], envelope.n);
    D means = ta_new_doubles(c, limits.n - 1);
    for (int i = 0; i < means.n; i++) {
        int start = limits.v[i] - 1, end = limits.v[i + 1] - 1;
        means.v[i] = end >= start ? sum(envelope, start, end) / (end - start + 1) : 0;
    }
    double reference = means.n > 2 ? sum(means, 1, means.n - 2) / (means.n - 2) : 0;
    bool omit = means.v[1] < reference * 0.5 && boundaries.n > 3;
    if (omit)
        reference = sum(means, 2, boundaries.n - 2) / (boundaries.n - 3);
    int first = omit ? 2 : 1;
    double threshold = omit ? 0.075 : 0.09;
    D deviations = ta_new_doubles(c, means.n);
    deviations.n = 0;
    for (int i = first; i < means.n - 1; i++)
        if (limits.v[i + 1] - limits.v[i] > 16 || means.v[i] > reference * 0.7)
            deviations.v[deviations.n++] = fabs(reference - means.v[i]);
    double summary = deviations.n ? d_sequential(deviations) / deviations.n : 0;
    return (Variation){summary >= threshold, summary};
}
PhraseExport phrase_convert(ta_context *c, int mood, Decorated d, Variation variation) {
    int n = d.fills.roles.n;
    if (d.boundaries.n != n + 1 || d.fills.auxiliary.n != n || d.upStarts.n != n ||
        d.upFirst.n != n || d.upSecond.n != n || d.upThird.n != n)
        ta_fail(c, TA_FAILED, "phrase conversion dimensions");
    az_analysis_phrase_v1 *entries = ta_alloc(c, n, sizeof *entries);
    I auxiliary = i_copy(c, d.fills.auxiliary), materialRoles = ta_new_ints(c, n),
      materialKinds = ta_new_ints(c, n);
    int materials = 0, next = 1;
    const int cycle[] = {7, 2, 3, 4, 5, 6};
    for (int i = 0; i < n; i++) {
        az_analysis_phrase_v1 *p = &entries[i];
        p->start_beat = d.boundaries.v[i];
        p->fill_start_beat = d.fills.starts.v[i];
        int role = d.fills.roles.v[i];
        if (mood != MOOD_HIGH) {
            switch (role) {
            case 10:
                p->kind = 1;
                break;
            case 40:
                p->kind = 9;
                break;
            case 50:
                p->kind = 8;
                break;
            case 60:
                p->kind = 10;
                break;
            default:
                if (role >= 61) {
                    int index = 0;
                    while (index < materials && materialRoles.v[index] != role)
                        index++;
                    if (index == materials) {
                        materialRoles.v[index] = role;
                        materialKinds.v[index] = cycle[next++ % 6];
                        materials++;
                    }
                    p->kind = materialKinds.v[index];
                }
                break;
            }
            continue;
        }
        bool decorated = role > 99;
        if (!decorated)
            auxiliary.v[i] = 0;
        int family = decorated ? (int)(role * 0.01) : (int)(role * 0.1),
            remainder = decorated ? role - family * 100 : role - family * 10,
            style = decorated ? remainder % 10 : remainder;
        switch (family) {
        case 1:
        case 4:
            p->kind = family == 1 ? 1 : 5;
            if (decorated)
                p->fill = d.fills.auxiliary.v[i] != 1;
            if (style == 1)
                p->variant1 = 1;
            break;
        case 2:
            p->kind = 4;
            if (decorated)
                p->fill = 1;
            break;
        case 3:
            p->kind = 2;
            if (decorated)
                p->fill = d.fills.auxiliary.v[i] != 1;
            if (style == 1)
                p->variant3 = 1;
            else if (style == 2) {
                p->variant2 = 1;
                p->structure = 1;
                p->beat2 = d.upFirst.v[i];
                p->beat3 = d.upSecond.v[i];
                p->beat4 = d.upThird.v[i];
            } else if (style == 3) {
                p->variant2 = 1;
                p->beat2 = d.upStarts.v[i];
            }
            break;
        case 5:
            p->kind = 3;
            break;
        case 6:
            p->kind = 6;
            if ((decorated ? remainder / 10 : style) == 1)
                p->variant1 = 1;
            break;
        default:
            break;
        }
    }
    return (PhraseExport){{(uint32_t)mood, (uint32_t)d.boundaries.v[n], (uint32_t)n, 0, entries},
                          variation,
                          auxiliary};
}
PhraseExport export_non_high(ta_context *c, int mood, I b, Spectra spectra, int phase, int end,
                             D middle, D envelope, D vocal, D low, B activity, D quarters,
                             D power) {
    if (mood == MOOD_HIGH || !b.n)
        ta_fail(c, TA_FAILED, "non-high export dimensions");
    int n = b.n;
    I roles, temporary;
    D metric;
    Variation variation = {0};
    if (n == 1) {
        roles = ta_new_ints(c, 1);
        roles.v[0] = 10;
        temporary = roles;
        metric = ta_new_doubles(c, 1);
    } else {
        Recurrence recurrent = recurrence(c, spectra, b, phase, end);
        SeedEvidence seeds = non_high_seeds(c, middle, envelope, b, vocal, mood);
        roles = reconcile_roles(c, seeds.roles, recurrent);
        BlockMeans blocks = block_means(c, b, envelope, low, power, phase);
        temporary = high_classify(c, b, blocks.intro, blocks.low).roles;
        metric = blocks.low;
        variation = phrase_variation(c, envelope, b);
    }
    int terminal = phrase_outro(c, b, temporary, low, activity, quarters, phase).terminal;
    I zeros = ta_new_ints(c, n);
    Fills fills = {roles, zeros, zeros, zeros};
    I ends = i_join(c, b, (I){&terminal, 1});
    final_adjust(c, ends, false, metric, &fills);
    Decorated decorated = {ends, fills, zeros, zeros, zeros, zeros};
    return phrase_convert(c, mood, decorated, variation);
}
