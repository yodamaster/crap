/// @file
/// Handling of tag/branch fixups.  A tag (or start of a branch) may contain
/// differences from the state of the point we're placed it in the parent
/// branch.
///
/// Detect these, and insert fixup-commits as required.

#include "database.h"
#include "errno.h"
#include "file.h"
#include "fixup.h"
#include "log.h"
#include "utils.h"

#include <limits.h>
#include <stdlib.h>

// FIXME - assumes signed time_t!
#define TIME_MIN (sizeof(time_t) == sizeof(int) ? INT_MIN : LONG_MIN)
#define TIME_MAX (sizeof(time_t) == sizeof(int) ? INT_MAX : LONG_MAX)


static int compare_fixup_by_time (const void * AA, const void * BB)
{
    const fixup_ver_t * A = AA;
    const fixup_ver_t * B = BB;
    if (A->time < B->time)
        return -1;

    if (A->time > B->time)
        return 1;

    return 0;
}


void create_fixups(const database_t * db,
                   version_t * const * branch_versions, tag_t * tag)
{
    // Go through the current versions on the branch and note any version
    // fix-ups required.
    assert (tag->fixups == NULL);
    assert (tag->fixups_end == NULL);

    version_t ** tf = tag->tag_files;
    for (file_t * i = db->files; i != db->files_end; ++i) {
        version_t * bv = branch_versions ? version_normalise (
            branch_versions[i - db->files]) : NULL;
        version_t * tv = NULL;
        if (tf != tag->tag_files_end && (*tf)->file == i)
            tv = version_normalise (*tf++);

        version_t * bvl = bv == NULL || bv->dead ? NULL : bv;
        version_t * tvl = tv == NULL || tv->dead ? NULL : tv;

        if (bvl == tvl)
            continue;

        assert (TIME_MIN < 0);
        assert (TIME_MAX > 0);
        assert (TIME_MIN == (time_t) ((unsigned long long) TIME_MAX + 1));

        // FIXME need to worry about forcing version out before its successors!
        time_t fix_time;
        if (tv != NULL)
            fix_time = tv->time;
        else
            fix_time = TIME_MIN;

        ARRAY_APPEND (tag->fixups, ((fixup_ver_t) {
                    .file = i, .version = tvl, .time = fix_time }));
    }

    tag->fixups_curr = tag->fixups;

    // Sort fix-ups by date.
    ARRAY_SORT (tag->fixups, compare_fixup_by_time);
}


char * fixup_commit_comment (const database_t * db,
                             version_t * const * base_versions, tag_t * tag,
                             fixup_ver_t * fixups,
                             fixup_ver_t * fixups_end)
{
    // Generate stats.
    size_t keep = 0;
    size_t added = 0;
    size_t deleted = 0;
    size_t modified = 0;

    //version_t ** fetch = NULL;
    //version_t ** fetch_end = NULL;

    fixup_ver_t * ffv = fixups;
    for (file_t * i = db->files; i != db->files_end; ++i) {
        version_t * bv = base_versions ?
            version_live (base_versions[i - db->files]) : NULL;
        version_t * tv;
        if (ffv != fixups_end && ffv->file == i)
            tv = ffv++->version;
        else
            tv = bv;

        if (bv == tv) {
            if (bv != NULL)
                ++keep;
            continue;
        }

        if (tv == NULL) {
            ++deleted;
            continue;
        }

        //if (tv->mark == SIZE_MAX)
        //ARRAY_APPEND (fetch, tv);

        if (bv == NULL)
            ++added;
        else
            ++modified;
    }

    assert (added + deleted + modified == fixups_end - fixups);

    // FIXME - grab_versions assumes that all versions are on the same branch!
    // We should pass in the tag rather than guessing it!
    //grab_versions (db, s, fetch, fetch_end);
    //xfree (fetch);

    //tag->fixup = true;
    //tag->changeset.mark = ++mark_counter;

    // Generate the commit comment.
    char * result;
    size_t res_size;

    FILE * f = open_memstream(&result, &res_size);
    if (f == NULL)
        fatal ("open_memstream failed: %s\n", strerror (errno));

    fprintf (f, "Fix-up commit generated by crap-clone.  "
             "(~%zu +%zu -%zu =%zu)\n", modified, added, deleted, keep);

    ffv = fixups;
    for (file_t * i = db->files; i != db->files_end; ++i) {
        version_t * bv = base_versions ?
            version_live (base_versions[i - db->files]) : NULL;
        version_t * tv = NULL;
        if (ffv != fixups_end && ffv->file == i)
            tv = ffv++->version;
        else
            tv = bv;

        if (bv == tv) {
            if (bv != NULL && keep <= deleted)
                fprintf (f, "%s KEEP %s\n", bv->file->path, bv->version);
            continue;
        }

        if (tv != NULL || deleted <= keep)
            fprintf (f, "%s %s->%s\n", i->path,
                     bv ? bv->version : "ADD", tv ? tv->version : "DELETE");
    }

    if (ferror (f))
        fatal ("memstream: error creating log message\n");

    fclose (f);

    return result;
}
