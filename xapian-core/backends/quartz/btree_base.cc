/* btree_base.cc: Btree base file implementation
 *
 * Copyright 1999,2000,2001 BrightStation PLC
 * Copyright 2002,2003,2004,2006 Olly Betts
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301
 * USA
 */

#include <config.h>

#include "safeerrno.h"

#include "btree_base.h"
#include "quartz_utils.h"
#include "utils.h"
#include <xapian/error.h>
#include "omassert.h"

#include <limits.h>

#include <algorithm>

using namespace std;

/************ Base file parameters ************/

/** This is the current description of the base file format:
 *
 * Numbers are (unless mentioned otherwise) stored in the variable
 * length format used by pack_uint() - that is 7 bits at a time in
 * a byte, starting with lower-order bits, and setting the high bit
 * on all bytes before the last one.
 *
 * The format consists of a sequence of numbers in this order:
 *
 * REVISION
 * FORMAT	will be = CURR_FORMAT for the current format.  If this value
 * 		higher then it is a different format which we
 * 		doesn't yet understand, so we bomb out.  If it's lower,
 * 		then it depends if we have backwards-compatibility code
 * 		implemented (we don't for format versions < 5).
 * BLOCK_SIZE
 * ROOT
 * LEVEL
 * BIT_MAP_SIZE
 * ITEM_COUNT
 * LAST_BLOCK
 * HAVE_FAKEROOT
 * SEQUENTIAL
 * REVISION2	A second copy of the revision number, for consistency checks.
 * BITMAP	The bitmap.  This will be BIT_MAP_SIZE raw bytes.
 * REVISION3	A third copy of the revision number, for consistency checks.
 */
#define CURR_FORMAT 5U

Btree_base::Btree_base()
	: revision(0),
	  block_size(0),
	  root(0),
	  level(0),
	  bit_map_size(0),
	  item_count(0),
	  last_block(0),
	  have_fakeroot(false),
	  sequential(false),
	  bit_map_low(0),
	  bit_map0(0),
	  bit_map(0)
{
}

Btree_base::Btree_base(const string &name_, char ch)
	: revision(0),
	  block_size(0),
	  root(0),
	  level(0),
	  bit_map_size(0),
	  item_count(0),
	  last_block(0),
	  have_fakeroot(false),
	  sequential(false),
	  bit_map_low(0),
	  bit_map0(0),
	  bit_map(0)
{
    string err_msg;
    if (!read(name_, ch, err_msg)) {
	throw Xapian::DatabaseOpeningError(err_msg);
    }
}

Btree_base::Btree_base(const Btree_base &other)
	: revision(other.revision),
	  block_size(other.block_size),
	  root(other.root),
	  level(other.level),
	  bit_map_size(other.bit_map_size),
	  item_count(other.item_count),
	  last_block(other.last_block),
	  have_fakeroot(other.have_fakeroot),
	  sequential(other.sequential),
	  bit_map_low(other.bit_map_low),
	  bit_map0(0),
	  bit_map(0)
{
    try {
	bit_map0 = new byte[bit_map_size];
	bit_map = new byte[bit_map_size];

	memcpy(bit_map0, other.bit_map0, bit_map_size);
	memcpy(bit_map, other.bit_map, bit_map_size);
    } catch (...) {
	delete [] bit_map0;
	delete [] bit_map;
    }
}

void
Btree_base::swap(Btree_base &other)
{
    std::swap(revision, other.revision);
    std::swap(block_size, other.block_size);
    std::swap(root, other.root);
    std::swap(level, other.level);
    std::swap(bit_map_size, other.bit_map_size);
    std::swap(item_count, other.item_count);
    std::swap(last_block, other.last_block);
    std::swap(have_fakeroot, other.have_fakeroot);
    std::swap(sequential, other.sequential);
    std::swap(bit_map_low, other.bit_map_low);
    std::swap(bit_map0, other.bit_map0);
    std::swap(bit_map, other.bit_map);
}

Btree_base::~Btree_base()
{
    delete [] bit_map;
    bit_map = 0;
    delete [] bit_map0;
    bit_map0 = 0;
}

bool
Btree_base::do_unpack_uint(const char **start, const char *end,
			   uint4 *dest, string &err_msg, 
			   const string &basename,
			   const char *varname)
{
    bool result = unpack_uint(start, end, dest);
    if (!result) {
	err_msg += "Unable to read " + string(varname) + " from " +
		    basename + "\n";
    }
    return result;
}

#define DO_UNPACK_UINT_ERRCHECK(start, end, var) \
do { \
    if (!do_unpack_uint(start, end, &var, err_msg, basename, #var)) { \
	return false; \
    } \
} while(0)

/* How much of the base file to read at the first go (in bytes).
 * This should be big enough that the base file without bitmap
 * will fit in to this size with no problem.  Other than that
 * it's fairly arbitrary, but shouldn't be big enough to cause
 * serious memory problems!
 */
#define REASONABLE_BASE_SIZE 1024

bool
Btree_base::read(const string & name, char ch, string &err_msg)
{
    string basename = name + "base" + ch;
    int h = sys_open_to_read_no_except(basename);
    fdcloser closefd(h);
    if (h == -1) {
	err_msg += "Couldn't open " + basename + ": " + strerror(errno) + "\n";
	return false;
    }
    string buf(sys_read_all_bytes(h, REASONABLE_BASE_SIZE));

    const char *start = buf.data();
    const char *end = start + buf.length();

    DO_UNPACK_UINT_ERRCHECK(&start, end, revision);
    uint4 format;
    DO_UNPACK_UINT_ERRCHECK(&start, end, format);
    if (format != CURR_FORMAT) {
	err_msg += "Bad base file format " + om_tostring(format) + " in " +
		    basename + "\n";
	return false;
    }
    DO_UNPACK_UINT_ERRCHECK(&start, end, block_size);
    DO_UNPACK_UINT_ERRCHECK(&start, end, root);
    DO_UNPACK_UINT_ERRCHECK(&start, end, level);
    DO_UNPACK_UINT_ERRCHECK(&start, end, bit_map_size);
    /* Now that we know the size of the bit map, (possibly)
     * read in more data from the base file in case
     * REASONABLE_BASE_SIZE is too small.  We need to update
     * start and end.
     */
    {
	unsigned long start_offset = start - buf.data();
	buf += sys_read_all_bytes(h, bit_map_size);
	start = buf.data() + start_offset;
	end = buf.data() + buf.length();
    }
    DO_UNPACK_UINT_ERRCHECK(&start, end, item_count);
    DO_UNPACK_UINT_ERRCHECK(&start, end, last_block);
    uint4 have_fakeroot_;
    DO_UNPACK_UINT_ERRCHECK(&start, end, have_fakeroot_);
    have_fakeroot = have_fakeroot_;

    uint4 sequential_;
    DO_UNPACK_UINT_ERRCHECK(&start, end, sequential_);
    sequential = sequential_;

    if (have_fakeroot && !sequential) {
	sequential = true; // FIXME : work out why we need this...
	/*
	err_msg += "Corrupt base file, `" + basename + "':\n"
		"sequential must be set whenever have_fakeroot is set.\n" +
		"sequential=" + (sequential?"true":"false") +
		", have_fakeroot=" + (have_fakeroot?"true":"false") + "\n";
	return false;
	*/
    }

    uint4 revision2;
    DO_UNPACK_UINT_ERRCHECK(&start, end, revision2);
    if (revision != revision2) {
	err_msg += "Revision number mismatch in " +
		basename + ": " +
		om_tostring(revision) + " vs " + om_tostring(revision2) + "\n";
	return false;
    }

    /* Read the bitmap */
    if (uint4(end - start) <= bit_map_size) {
	err_msg += "Not enough space for bitmap in base file " +
		basename + "\n";
	return false;
    }

    /* It's ok to delete a zero pointer */
    delete [] bit_map0;
    bit_map0 = 0;
    delete [] bit_map;
    bit_map = 0;

    bit_map0 = new byte[bit_map_size];
    bit_map = new byte[bit_map_size];
    memcpy(bit_map0, start, bit_map_size);
    memcpy(bit_map, bit_map0, bit_map_size);
    start += bit_map_size;

    uint4 revision3;
    if (!unpack_uint(&start, end, &revision3)) {
	err_msg += "Couldn't read revision2 from base file " +
	basename + "\n";
	return false;
    }

    if (revision != revision3) {
	err_msg += "Revision number mismatch in " +
		basename + ": " +
		om_tostring(revision) + " vs " + om_tostring(revision2) + "\n";
	return false;
    }

    if (start != end) {
	err_msg += "Junk at end of base file " + basename + "\n";
	return false;
    }

    return true;
}

void
Btree_base::write_to_file(const string &filename)
{
    calculate_last_block();

    string buf;
    buf += pack_uint(revision);
    buf += pack_uint(CURR_FORMAT);
    buf += pack_uint(block_size);
    buf += pack_uint(static_cast<uint4>(root));
    buf += pack_uint(static_cast<uint4>(level));
    buf += pack_uint(static_cast<uint4>(bit_map_size));
    buf += pack_uint(static_cast<uint4>(item_count));
    buf += pack_uint(static_cast<uint4>(last_block));
    buf += pack_uint(have_fakeroot);
    buf += pack_uint(sequential);
    buf += pack_uint(revision);
    if (bit_map_size) {
	buf.append(reinterpret_cast<const char *>(bit_map), bit_map_size);
    }
    buf += pack_uint(revision);  // REVISION2

    int h = sys_open_to_write(filename);
    fdcloser closefd(h);

    sys_write_string(h, buf);
    sys_flush(h);
}

/*
   block_free_at_start(B, n) is true iff (if and only if) block n was free at
   the start of the transaction on the B-tree.
*/

bool
Btree_base::block_free_at_start(uint4 n) const
{
    size_t i = n / CHAR_BIT;
    int bit = 0x1 << n % CHAR_BIT;
    return (bit_map0[i] & bit) == 0;
}

/* free_block(B, n) causes block n to be marked free in the bit map.
   B->bit_map_low is the lowest byte in the bit map known to have a free bit
   set. Searching starts from there when looking for a free block.
*/

void
Btree_base::free_block(uint4 n)
{
    uint4 i = n / CHAR_BIT;
    int bit = 0x1 << n % CHAR_BIT;
    bit_map[i] &= ~ bit;

    if (bit_map_low > i)
	if ((bit_map0[i] & bit) == 0) /* free at start */
	    bit_map_low = i;
}

/* extend(B) increases the size of the two bit maps in an obvious way.
   The bitmap file grows and shrinks as the DB file grows and shrinks in
   internal usage. But the DB file itself does not reduce in size, no matter
   how many blocks are freed.
*/

#define BIT_MAP_INC 1000
    /* increase the bit map by this number of bytes if it overflows */

void
Btree_base::extend_bit_map()
{
    int n = bit_map_size + BIT_MAP_INC;
    byte *new_bit_map0 = 0;
    byte *new_bit_map = 0;

    try {
	new_bit_map0 = new byte[n];
	new_bit_map = new byte[n];

	memcpy(new_bit_map0, bit_map0, bit_map_size);
	memset(new_bit_map0 + bit_map_size, 0, n - bit_map_size);
	
	memcpy(new_bit_map, bit_map, bit_map_size);
	memset(new_bit_map + bit_map_size, 0, n - bit_map_size);
    } catch (...) {
	delete [] new_bit_map0;
	delete [] new_bit_map;
	throw;
    }
    delete [] bit_map0;
    bit_map0 = new_bit_map0;
    delete [] bit_map;
    bit_map = new_bit_map;
    bit_map_size = n;
}

/* next_free_block(B) returns the number of the next available free block in
   the bitmap, marking it as 'in use' before returning. More precisely, we get
   a block that is both free now (in bit_map) and was free at the beginning of
   the transaction on the B-tree (in bit_map0).

   Starting at bit_map_low we go up byte at a time until we find a byte with a
   free (zero) bit, and then go up that byte bit at a time. If the bit map has
   no free bits it is extended so that it will have.
*/

uint4
Btree_base::next_free_block()
{
    uint4 i;
    int x;
    for (i = bit_map_low;; i++)
    {  
	if (i >= bit_map_size) {
	    extend_bit_map();
	}
        x = bit_map0[i] | bit_map[i];
        if (x != UCHAR_MAX) break;
    }
    uint4 n = i * CHAR_BIT;
    int d = 0x1;
    while ((x & d) != 0) { d <<= 1; n++; }
    bit_map[i] |= d;   /* set as 'in use' */
    bit_map_low = i;
    if (n > last_block) {
	last_block = n;
    }
    return n;
}

bool
Btree_base::block_free_now(uint4 n)
{
    uint4 i = n / CHAR_BIT;
    int bit = 0x1 << n % CHAR_BIT;
    return (bit_map[i] & bit) == 0;
}

void
Btree_base::calculate_last_block()
{
    if (bit_map_size == 0) {
	last_block = 0;
	return;
    }
    int i = bit_map_size - 1;
    while (bit_map[i] == 0 && i > 0) {
	i--;
    }
    bit_map_size = i + 1;

    int x = bit_map[i];

    /* Check for when there are no blocks */
    if (x == 0) {
	last_block = 0;
	return;
    }
    uint4 n = (i + 1) * CHAR_BIT - 1;
    int d = 0x1 << (CHAR_BIT - 1);
    while ((x & d) == 0) { d >>= 1; n--; }

    last_block = n;
}

bool
Btree_base::is_empty() const
{
    for (uint4 i = 0; i < bit_map_size; i++) {
	if (bit_map[i] != 0) {
	    return false;
	}
    }
    return true;
}

void
Btree_base::clear_bit_map()
{
    memset(bit_map, 0, bit_map_size);
}

// We've commited, so "bitmap at start" needs to be reset to the current bitmap.
void
Btree_base::commit()
{
    memcpy(bit_map0, bit_map, bit_map_size);
    bit_map_low = 0;
}
