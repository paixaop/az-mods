/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct { double points[4]; int count; } Curve;
const char *const sp_curve_names[SP_CURVES] = {
    "Opening", "Primetime", "Closing", "Full night", "B2B"
};
static const Curve curves[SP_CURVES] = {
    {{2, 4, 6}, 3}, {{6, 10, 8}, 3}, {{8, 5, 3}, 3},
    {{3, 7, 10, 5}, 4}, {{5, 7, 6}, 3}
};

static bool interrupted(const SpOptions *options)
{
    return options->cancelled && options->cancelled(options->user);
}

static double curve_at(const Curve *curve, double position)
{
    double index = sp_clamp(position, 0, 1) * (curve->count - 1);
    int left = (int)index;
    int right = left + (left < curve->count - 1);
    return curve->points[left] + (index - left) * (curve->points[right] - curve->points[left]);
}

static bool note_mode(const char *suffix, bool *major)
{
    while (isspace((unsigned char)*suffix))
        ++suffix;
    if (strcmp(suffix, "m") == 0 || strcasecmp(suffix, "minor") == 0 ||
        strcasecmp(suffix, "min") == 0) {
        *major = false;
        return true;
    }
    *major = !*suffix || strcasecmp(suffix, "maj") == 0 ||
        strcasecmp(suffix, "major") == 0 || strcmp(suffix, "M") == 0;
    return *major;
}

static int accidental(const char **suffix)
{
    const char *text = *suffix;
    if (*text == '#') {
        ++*suffix;
        return 1;
    }
    if (*text == 'b') {
        ++*suffix;
        return -1;
    }
    if (strncmp(text, "♯", 3) == 0) {
        *suffix += 3;
        return 1;
    }
    if (strncmp(text, "♭", 3) == 0) {
        *suffix += 3;
        return -1;
    }
    return 0;
}

static int musical_key(const char *key, bool *major)
{
    static const int notes[] = { 9, 11, 0, 2, 4, 5, 7 };
    static const int camelot_minor[] = {5,12,7,2,9,4,11,6,1,8,3,10};
    static const int camelot_major[] = {8,3,10,5,12,7,2,9,4,11,6,1};
    int note = toupper((unsigned char)*key) - 'A';
    if (note < 0 || note > 6)
        return 0;
    const char *suffix = key + 1;
    int semitone = notes[note] + accidental(&suffix);
    if (!note_mode(suffix, major))
        return 0;
    return (*major ? camelot_major : camelot_minor)[(semitone + 12) % 12];
}

static int camelot_key(const char *key, bool *major)
{
    char *end;
    long position = strtol(key, &end, 10);
    if (end == key || position < 1 || position > 12 || !end[0] || end[1])
        return 0;
    int mode = toupper((unsigned char)end[0]);
    if (mode != 'A' && mode != 'B')
        return 0;
    *major = mode == 'B';
    return (int)position;
}

void sp_track_key(SpTrack *track, const char *key)
{
    track->key = 0;
    track->major = false;
    if (!key)
        return;
    while (isspace((unsigned char)*key))
        ++key;
    track->key = camelot_key(key, &track->major);
    if (!track->key)
        track->key = musical_key(key, &track->major);
}

static int key_steps(const SpTrack *a, const SpTrack *b)
{
    int distance = abs(a->key - b->key);
    return distance > 6 ? 12 - distance : distance;
}

static double key_distance(const SpTrack *a, const SpTrack *b)
{
    if (!a->key || !b->key)
        return 3.0;
    static const double cosine[] = {1, 0.8660254037844386, 0.5, 0, -0.5, -0.8660254037844386, -1};
    double first = a->major ? 1.0 : 0.7;
    double second = b->major ? 1.0 : 0.7;
    return sqrt(fmax(0, first * first + second * second -
                        2 * first * second * cosine[key_steps(a, b)]));
}

double sp_tempo_gap(const SpTrack *a, const SpTrack *b, bool halftime)
{
    if (a->bpm <= 0 || b->bpm <= 0)
        return NAN;
    double direct = fabs(a->bpm - b->bpm);
    if (!halftime)
        return direct;
    return fmin(direct, fmin(fabs(a->bpm * 2 - b->bpm), fabs(a->bpm - b->bpm * 2)));
}

SpTransition sp_transition(const SpTrack *a, const SpTrack *b, const SpOptions *options)
{
    int steps = a->key && b->key ? key_steps(a, b) : -1;
    double bpm = sp_tempo_gap(a, b, options->halftime);
    return (SpTransition){.bpm_gap = bpm, .key_steps = steps,
        .tempo_warning = bpm > options->max_bpm, .key_warning = steps >= 4};
}

static int compare_number(const void *left, const void *right)
{
    double a = *(const double *)left, b = *(const double *)right;
    return (a > b) - (a < b);
}

static int collect_energy(const SpTrack *tracks, int count, double *valid)
{
    int size = 0;
    for (int i = 0; i < count; ++i)
        if (tracks[i].energy_known)
            valid[size++] = tracks[i].energy;
    return size;
}

bool sp_normalize_energy(SpTrack *tracks, int count)
{
    double *valid = calloc((size_t)count, sizeof *valid);
    if (!valid)
        return false;
    int size = collect_energy(tracks, count, valid);
    qsort(valid, (size_t)size, sizeof *valid, compare_number);
    double low = size ? valid[(size - 1) / 10] : 0;
    double high = size ? valid[size - 1 - (size - 1) / 10] : 0;
    for (int i = 0; i < count; ++i)
        tracks[i].energy = tracks[i].energy_known && high > low
            ? 1 + 9 * sp_clamp((tracks[i].energy - low) / (high - low), 0, 1) : 5.0;
    free(valid);
    return true;
}

static void measure_transition(SpMetrics *m, const SpTrack *a, const SpTrack *b,
                               const SpOptions *options)
{
    SpTransition transition = sp_transition(a, b, options);
    int steps = transition.key_steps;
    if (steps < 0) {
        ++m->unknown_keys;
        m->score += 100;
    } else if (transition.key_warning) {
        ++m->key_warnings;
        m->score += steps == 6 ? 100 : 50;
    } else {
        m->compatible += steps <= 1;
        m->score += 10 * key_distance(a, b);
    }
    double bpm = transition.bpm_gap;
    if (!isfinite(bpm)) {
        ++m->unknown_bpm;
        return;
    }
    m->max_bpm = fmax(m->max_bpm, bpm);
    m->max_raw_bpm = fmax(m->max_raw_bpm, fabs(a->bpm - b->bpm));
    if (transition.tempo_warning) {
        ++m->tempo_jumps;
        m->score += 10000 + 100 * (bpm - options->max_bpm);
    } else {
        m->score += bpm * bpm;
    }
}

static double tempo_flow(const SpTrack *a, const SpTrack *b, const SpTrack *c)
{
    if (a->bpm <= 0 || b->bpm <= 0 || c->bpm <= 0)
        return 0;
    double before = b->bpm - a->bpm, after = c->bpm - b->bpm;
    return before * after < 0 ? 2 * fmin(fabs(before), fabs(after)) : 0;
}

static double energy_flow(const SpTrack *a, const SpTrack *b, const SpTrack *c)
{
    double before = b->energy - a->energy, after = c->energy - b->energy;
    return before * after < 0 && fabs(after) > 1.5 ? 0.5 * fabs(after) : 0;
}

static int track_at(const int *order, int position)
{
    return order ? order[position] : position;
}

static double set_duration(const SpTrack *tracks, int count, const SpOptions *options)
{
    double duration = 0;
    if (options->time_aware)
        for (int i = 0; i < count; ++i)
            duration += tracks[i].duration;
    return duration;
}

SpMetrics sp_measure(const SpTrack *tracks, int count, const int *order,
                      const SpOptions *options)
{
    SpMetrics metrics = {0};
    if (count < 2)
        return metrics;
    const Curve *curve = &curves[options->curve];
    double duration = set_duration(tracks, count, options), elapsed = 0;
    for (int i = 0; i < count; ++i) {
        const SpTrack *track = &tracks[track_at(order, i)];
        double position = duration > 0
            ? (elapsed + track->duration / 2) / duration : (double)i / (count - 1);
        double delta = track->energy - curve_at(curve, position);
        metrics.score += 1.5 * delta * delta;
        elapsed += track->duration;
        if (i > 0)
            measure_transition(&metrics, &tracks[track_at(order, i - 1)], track, options);
        if (i > 1) {
            const SpTrack *a = &tracks[track_at(order, i - 2)];
            const SpTrack *b = &tracks[track_at(order, i - 1)];
            metrics.score += tempo_flow(a, b, track) + energy_flow(a, b, track);
        }
    }
    return metrics;
}

typedef struct { double energy, tempo; } Point;

static Point features(const SpTrack *track)
{
    return (Point){0.15 * track->energy, track->bpm > 0
        ? sp_clamp((track->bpm - 80) / 100, 0, 1) : 0.5};
}

static double separation(Point a, Point b)
{
    double energy = a.energy - b.energy, tempo = a.tempo - b.tempo;
    return energy * energy + tempo * tempo;
}

static Point combine(Point a, int a_count, Point b, int b_count)
{
    return (Point){(a.energy * a_count + b.energy * b_count) / (a_count + b_count),
                   (a.tempo * a_count + b.tempo * b_count) / (a_count + b_count)};
}

typedef struct { int left, right; } WardPair;

static WardPair closest_pair(const Point *means, const int *size, int count)
{
    WardPair pair = {0};
    double best = INFINITY;
    for (int i = 0; i < count; ++i) {
        if (!size[i])
            continue;
        for (int j = i + 1; j < count; ++j) {
            if (!size[j])
                continue;
            double cost = separation(means[i], means[j]) * size[i] * size[j] / (size[i] + size[j]);
            if (cost < best) {
                best = cost;
                pair = (WardPair){i, j};
            }
        }
    }
    return pair;
}

static void compact_labels(const int *size, int *labels, int count)
{
    int compact[50], next = 0;
    for (int i = 0; i < count; ++i)
        if (size[i])
            compact[i] = next++;
    for (int i = 0; i < count; ++i)
        labels[i] = compact[labels[i]];
}

static void ward_clusters(const Point *points, int count, int groups, int *labels)
{
    Point means[50];
    int size[50];
    for (int i = 0; i < count; ++i) {
        means[i] = points[i];
        labels[i] = i;
        size[i] = 1;
    }
    for (int remaining = count; remaining > groups; --remaining) {
        WardPair pair = closest_pair(means, size, count);
        int left = pair.left, right = pair.right;
        means[left] = combine(means[left], size[left], means[right], size[right]);
        size[left] += size[right];
        size[right] = 0;
        for (int i = 0; i < count; ++i)
            if (labels[i] == right)
                labels[i] = left;
    }
    compact_labels(size, labels, count);
}

static uint32_t random_u32(uint32_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

static double random_unit(uint32_t *state)
{
    return (double)random_u32(state) / 4294967296.0;
}

static void seed_centers(const Point *points, int count, Point *centers, int groups,
                         uint32_t *seed)
{
    centers[0] = points[random_u32(seed) % (uint32_t)count];
    for (int k = 1; k < groups; ++k) {
        int farthest = 0;
        double largest = -1;
        for (int i = 0; i < count; ++i) {
            double nearest = INFINITY;
            for (int g = 0; g < k; ++g)
                nearest = fmin(nearest, separation(points[i], centers[g]));
            if (nearest > largest) {
                largest = nearest;
                farthest = i;
            }
        }
        centers[k] = points[farthest];
    }
}

static double kmeans_assign(const Point *points, int count, Point *centers, int groups,
                            int *labels, bool *changed)
{
    Point sums[8] = {0};
    int sizes[8] = {0};
    double total = 0;
    *changed = false;
    for (int i = 0; i < count; ++i) {
        int best = 0;
        double cost = separation(points[i], centers[0]);
        for (int g = 1; g < groups; ++g) {
            double current = separation(points[i], centers[g]);
            if (current < cost) {
                best = g;
                cost = current;
            }
        }
        *changed |= labels[i] != best;
        labels[i] = best;
        sums[best].energy += points[i].energy;
        sums[best].tempo += points[i].tempo;
        ++sizes[best];
        total += cost;
    }
    for (int g = 0; g < groups; ++g)
        if (sizes[g])
            centers[g] = (Point){ sums[g].energy / sizes[g], sums[g].tempo / sizes[g] };
    return total;
}

static bool kmeans_clusters(const Point *points, int count, int groups, int *labels,
                            const SpOptions *options)
{
    uint32_t seed = 42;
    double best = INFINITY;
    int *candidate = calloc((size_t)count, sizeof *candidate);
    if (!candidate)
        return false;
    for (int attempt = 0; attempt < 10 && !interrupted(options); ++attempt) {
        Point centers[8];
        seed_centers(points, count, centers, groups, &seed);
        for (int i = 0; i < count; ++i)
            candidate[i] = -1;
        double score = 0;
        for (int iteration = 0; iteration < 64 && !interrupted(options); ++iteration) {
            bool changed;
            score = kmeans_assign(points, count, centers, groups, candidate, &changed);
            if (!changed)
                break;
        }
        if (score < best) {
            best = score;
            memcpy(labels, candidate, (size_t)count * sizeof *labels);
        }
    }
    free(candidate);
    return !interrupted(options);
}

static void reverse(int *order, int first, int last)
{
    while (first < last) {
        int track = order[first];
        order[first++] = order[last];
        order[last--] = track;
    }
}

static void nearest_cluster(const SpTrack *tracks, int *members, int count,
                            const SpOptions *options)
{
    for (int i = 1; i < count && !interrupted(options); ++i) {
        int best = i;
        double dist = INFINITY;
        for (int j = i; j < count; ++j) {
            double cost = key_distance(&tracks[members[i - 1]], &tracks[members[j]]);
            if (cost < dist) {
                best = j;
                dist = cost;
            }
        }
        int saved = members[i];
        members[i] = members[best];
        members[best] = saved;
    }
}

static bool shorter_edges(const SpTrack *tracks, const int *members, int i, int j)
{
    const SpTrack *a = &tracks[members[i - 1]], *b = &tracks[members[i]];
    const SpTrack *c = &tracks[members[j]], *d = &tracks[members[j + 1]];
    return key_distance(a, c) + key_distance(b, d) + 1e-9 <
           key_distance(a, b) + key_distance(c, d);
}

static bool improve_cluster(const SpTrack *tracks, int *members, int count,
                            const SpOptions *options)
{
    bool improved = false;
    for (int i = 1; i + 2 < count && !interrupted(options); ++i)
        for (int j = i + 1; j + 1 < count; ++j)
            if (shorter_edges(tracks, members, i, j)) {
                reverse(members, i, j);
                improved = true;
            }
    return improved;
}

static void arrange_cluster(const SpTrack *tracks, int *members, int count,
                            const SpOptions *options)
{
    nearest_cluster(tracks, members, count, options);
    for (int pass = 0; pass < 12 && !interrupted(options); ++pass)
        if (!improve_cluster(tracks, members, count, options))
            break;
}

static int cluster_entry(const SpTrack *tracks, const int *members, int count, int predecessor)
{
    if (predecessor < 0)
        return 0;
    int best = 0;
    double distance = INFINITY;
    for (int i = 0; i < count; ++i) {
        double cost = key_distance(&tracks[predecessor], &tracks[members[i]]);
        if (cost < distance) {
            best = i;
            distance = cost;
        }
    }
    return best;
}

typedef struct {
    int size;
    double energy;
} Zone;

static int pick_zone(const Zone *zones, int groups, double target)
{
    int group = -1;
    double best = INFINITY;
    for (int i = 0; i < groups; ++i) {
        if (!zones[i].size)
            continue;
        double distance = fabs(zones[i].energy / zones[i].size - target);
        if (distance < best) {
            best = distance;
            group = i;
        }
    }
    return group;
}

static int zone_members(const int *pool, const int *labels, int count, int group, int *members)
{
    int written = 0;
    for (int i = 0; i < count; ++i)
        if (labels[i] == group)
            members[written++] = pool[i];
    return written;
}

static int zone_stats(const SpTrack *tracks, const int *pool, const int *labels,
                      int count, Zone *zones)
{
    int present = 0;
    for (int i = 0; i < count; ++i) {
        Zone *zone = &zones[labels[i]];
        present += zone->size == 0;
        ++zone->size;
        zone->energy += tracks[pool[i]].energy;
    }
    return present;
}

typedef struct {
    const SpTrack *tracks;
    const SpOptions *options;
    const int *pool, *labels;
    int *members, *out;
    int groups, count;
} ClusterPlan;

static void arrange_zones(const ClusterPlan *plan)
{
    Zone zones[8] = {0};
    const SpOptions *options = plan->options;
    int present = zone_stats(plan->tracks, plan->pool, plan->labels, plan->count, zones);
    const Curve *curve = &curves[options->curve];
    int written = 0;
    for (int step = 0; step < present && !interrupted(options); ++step) {
        double position = present == 1 ? 0.5 : (double)step / (present - 1);
        int group = pick_zone(zones, plan->groups, curve_at(curve, position));
        int size = zone_members(plan->pool, plan->labels, plan->count, group, plan->members);
        arrange_cluster(plan->tracks, plan->members, size, options);
        int predecessor = written ? plan->out[written - 1] : options->opener;
        int entry = cluster_entry(plan->tracks, plan->members, size, predecessor);
        for (int i = 0; i < size; ++i)
            plan->out[written++] = plan->members[(entry + i) % size];
        zones[group].size = 0;
    }
}

static bool cluster_labels(const ClusterPlan *plan, int *labels)
{
    if (plan->groups == 1)
        return true;
    Point *points = calloc((size_t)plan->count, sizeof *points);
    if (!points)
        return false;
    for (int i = 0; i < plan->count; ++i)
        points[i] = features(&plan->tracks[plan->pool[i]]);
    bool result = true;
    if (plan->count <= 50)
        ward_clusters(points, plan->count, plan->groups, labels);
    else
        result = kmeans_clusters(points, plan->count, plan->groups, labels, plan->options);
    free(points);
    return result;
}

static bool cluster_order(const SpTrack *tracks, const SpOptions *options,
                          const int *pool, int count, int *out)
{
    int *work = calloc((size_t)count * 2, sizeof *work);
    if (!work)
        return false;
    int groups = count < 15 ? 1 : (int)fmin(8, sqrt(count));
    ClusterPlan plan = {tracks, options, pool, work, work + count, out, groups, count};
    bool result = cluster_labels(&plan, work);
    if (result)
        arrange_zones(&plan);
    free(work);
    return result && !interrupted(options);
}

static void anneal(const SpTrack *tracks, int count, const SpOptions *options,
                   int *candidate, int *best)
{
    int first = options->opener >= 0;
    int movable = count - first - (options->closer >= 0);
    size_t bytes = (size_t)count * sizeof *best;
    double current = sp_measure(tracks, count, candidate, options).score;
    double best_score = sp_measure(tracks, count, best, options).score;
    if (current < best_score) {
        memcpy(best, candidate, bytes);
        best_score = current;
    }
    if (movable < 2)
        return;
    uint32_t seed = 0x6f15a771;
    double temperature = 100;
    for (int iteration = 0; iteration < 1000 && !interrupted(options); ++iteration) {
        int lo = first + (int)(random_u32(&seed) % (uint32_t)(movable - 1));
        int hi = lo + 1 + (int)(random_u32(&seed) % (uint32_t)(first + movable - lo - 1));
        reverse(candidate, lo, hi);
        double score = sp_measure(tracks, count, candidate, options).score;
        if (score <= current || random_unit(&seed) < exp((current - score) / temperature)) {
            current = score;
            if (score < best_score) {
                best_score = score;
                memcpy(best, candidate, bytes);
            }
        } else {
            reverse(candidate, lo, hi);
        }
        temperature *= 0.995;
    }
}

static int anchored_order(int count, const SpOptions *options, int *out, int *first)
{
    int next = 0;
    if (options->opener >= 0)
        out[next++] = options->opener;
    *first = next;
    for (int i = 0; i < count; ++i)
        if (i != options->opener && i != options->closer)
            out[next++] = i;
    if (options->closer >= 0)
        out[next] = options->closer;
    return next - *first;
}

bool sp_plan_order(const SpTrack *tracks, int count, const SpOptions *options, int *out)
{
    if (count < 1)
        return false;
    if (count == 1) {
        out[0] = 0;
        return true;
    }
    int first;
    int pool_count = anchored_order(count, options, out, &first);
    if (!pool_count)
        return true;
    size_t bytes = (size_t)count * sizeof *out;
    int *candidate = malloc(bytes);
    if (!candidate)
        return false;
    memcpy(candidate, out, bytes);
    bool result = cluster_order(tracks, options, out + first, pool_count, candidate + first);
    if (result)
        anneal(tracks, count, options, candidate, out);
    free(candidate);
    return result && !interrupted(options);
}

void sp_relocate(int *order, int from, int to)
{
    int entry = order[from];
    if (from < to)
        memmove(order + from, order + from + 1, (size_t)(to - from) * sizeof *order);
    else
        memmove(order + to + 1, order + to, (size_t)(from - to) * sizeof *order);
    order[to] = entry;
}

typedef struct { int rank, end, previous, entry; bool keep; } MoveRow;

static int find_lis_end(const MoveRow *rows, int longest, int rank)
{
    int low = 0, high = longest;
    while (low < high) {
        int mid = low + (high - low) / 2;
        if (rows[rows[mid].end].rank < rank)
            low = mid + 1;
        else
            high = mid;
    }
    return low;
}

static void keep_lis(MoveRow *rows, const int *target, int count)
{
    int longest = 0;
    for (int i = 0; i < count; ++i)
        rows[target[i]].rank = i;
    for (int i = 0; i < count; ++i) {
        int next = find_lis_end(rows, longest, rows[i].rank);
        rows[i].entry = i;
        rows[i].previous = next ? rows[next - 1].end : -1;
        rows[next].end = i;
        longest += next == longest;
    }
    for (int i = rows[longest - 1].end; i >= 0; i = rows[i].previous)
        rows[i].keep = true;
}

static int entry_position(const MoveRow *rows, int entry)
{
    int i = 0;
    while (rows[i].entry != entry)
        ++i;
    return i;
}

static void move_between(MoveRow *rows, int from, int to, int entry)
{
    int step = from > to ? -1 : 1;
    for (int i = from; i != to; i += step)
        rows[i].entry = rows[i + step].entry;
    rows[to].entry = entry;
}

static int build_moves(MoveRow *rows, const int *target, int count, SpMove *out,
                       const SpOptions *options)
{
    int moves = 0;
    for (int i = count; i-- > 0 && !interrupted(options);) {
        int entry = target[i];
        if (rows[entry].keep)
            continue;
        int from = entry_position(rows, entry);
        int to = i + 1 < count ? entry_position(rows, target[i + 1]) : count;
        to -= from < to;
        if (from != to) {
            out[moves++] = (SpMove){entry, from, to};
            move_between(rows, from, to, entry);
        }
    }
    return moves;
}

int sp_plan_moves(const int *target, int count, SpMove *out, const SpOptions *options)
{
    if (count < 1)
        return 0;
    MoveRow *rows = calloc((size_t)count, sizeof *rows);
    if (!rows)
        return -1;
    keep_lis(rows, target, count);
    int moves = build_moves(rows, target, count, out, options);
    free(rows);
    return interrupted(options) ? -1 : moves;
}
