/*
 *  This file is part of Permafrost Engine.
 *  Copyright (C) 2026 Eduard Permyakov
 *
 *  Permafrost Engine is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Permafrost Engine is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  Linking this software statically or dynamically with other modules is making
 *  a combined work based on this software. Thus, the terms and conditions of
 *  the GNU General Public License cover the whole combination.
 *
 *  As a special exception, the copyright holders of Permafrost Engine give
 *  you permission to link Permafrost Engine with independent modules to produce
 *  an executable, regardless of the license terms of these independent
 *  modules, and to copy and distribute the resulting executable under
 *  terms of your choice, provided that you also meet, for each linked
 *  independent module, the terms and conditions of the license of that
 *  module. An independent module is a module which is not derived from
 *  or based on Permafrost Engine. If you modify Permafrost Engine, you may
 *  extend this exception to your version of Permafrost Engine, but you are not
 *  obliged to do so. If you do not wish to do so, delete this exception
 *  statement from your version.
 *
 */

#ifndef PF_COW_REGION_H
#define PF_COW_REGION_H

#include <stddef.h>
#include <stdbool.h>

/* A copy-on-write memory region exposing two views over one backing object:
 *
 *   - the WRITER view (pf_cow_writer_base): private and read-write. A write
 *     faults a private copy of the touched page, so it is invisible to the
 *     reader view and never alters the canonical backing.
 *   - the READER view (pf_cow_reader_base): the canonical state, read-only by
 *     convention. It stays frozen for as long as the caller refrains from
 *     calling pf_cow_publish, so any number of threads may read it concurrently
 *     with writes to the writer view.
 *
 * pf_cow_publish copies the given byte ranges from the writer into the
 * canonical (making them visible to readers), then resets the writer to mirror
 * the canonical again. It must be called when no thread is reading the reader
 * view. Only the ranges passed to publish survive the reset; any other writer
 * change is discarded, so the caller must publish every change it means to keep.
 *
 * The reader and writer live at different addresses, so anything stored in the
 * region must be position-independent (no pointers into the region itself).
 */

struct pf_cow_region;

struct cow_range{
    size_t off;
    size_t len;
};

/* Create a region of at least 'size' bytes, rounded up to a page and
 * zero-filled. Returns NULL on failure.
 */
struct pf_cow_region *pf_cow_create(size_t size);
void                  pf_cow_destroy(struct pf_cow_region *region);

/* The page-rounded size of the region. */
size_t      pf_cow_size(const struct pf_cow_region *region);

/* The writer (private read-write) and reader (canonical read-only) bases. The
 * writer base may change across a pf_cow_publish, so re-fetch it afterwards.
 */
void       *pf_cow_writer_base(struct pf_cow_region *region);
const void *pf_cow_reader_base(const struct pf_cow_region *region);

/* Commit 'ndirty' writer ranges to the canonical, then reset the writer.
 * Returns false on error, e.g. a range that falls outside the region.
 */
bool        pf_cow_publish(struct pf_cow_region *region,
                           const struct cow_range *dirty, size_t ndirty);

#endif /* PF_COW_REGION_H */
