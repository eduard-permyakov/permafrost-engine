/*
-- Macro: AC_STRUCT_ST_BLOCKS
    If 'struct stat' contains an 'st_blocks' member, define
    'HAVE_STRUCT_STAT_ST_BLOCKS'.  Otherwise, require an 'AC_LIBOBJ'
    replacement of 'fileblocks'.
*/

#if !HAVE_STRUCT_STAT_ST_BLOCKS
/* If necessary you may see gnulib for replacement function:
 * off_t st_blocks (off_t size).
 * You may found code available under GPL2 or GPL3.
 */

/* This declaration is solely to ensure that after preprocessing
   this file is never empty. */
typedef int textutils_fileblocks_unused;
#endif
