/* Read, sort and compare two directories.  Used for GNU DIFF.

   Copyright (C) 1988-1989, 1992-1995, 1998, 2001-2002, 2004, 2006-2007,
   2009-2013, 2015-2023 Free Software Foundation, Inc.

   This file is part of GNU DIFF.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "diff.h"
#include <error.h>
#include <dirname.h>
#include <exclude.h>
#include <filenamecat.h>
#include <setjmp.h>
#include <xalloc.h>

/* A sorted vector of file names obtained by reading a directory.  */

struct dirdata
{
  idx_t nnames;	/* Number of names.  */
  char const **names;	/* Sorted names of files in dir, followed by 0.  */
  char *data;	/* Allocated storage for file names.  */
};

/* Whether file names in directories should be compared with
   locale-specific sorting.  */
static bool locale_specific_sorting;

/* Where to go if locale-specific sorting fails.  */
static jmp_buf failed_locale_specific_sorting;

static int compare_names (char const *, char const *);
static bool dir_loop (struct comparison const *, int);


/* Given the parent directory PARENTDIRFD (negative for current dir),
   read the directory named by DIR and store into DIRDATA a sorted
   vector of filenames for its contents.
   Use DIR's basename if PARENTDIRFD is nonnegative, for efficiency.
   If DIR->desc == NONEXISTENT, this directory is known to be
   nonexistent so set DIRDATA to an empty vector;
   otherwise, update DIR->desc and DIR->dirstream as needed.
   If STARTFILE, ignore directory entries less than STARTFILE, and if
   STARTFILE_ONLY, also ignore directory entries greater than STARTFILE.
   Return true if successful, false (setting errno) otherwise.  */

static bool
dir_read (int parentdirfd, struct file_data *dir, struct dirdata *dirdata,
	  char const *startfile, bool startfile_only)
{
  /* Number of files in directory.  */
  idx_t nnames = 0;

  /* Allocated and used storage for file name data.  */
  char *data;

  dirdata->names = nullptr;
  dirdata->data = nullptr;

  if (dir->desc != NONEXISTENT)
    {
      /* Open the directory and check for errors.  */
      int dirfd = dir->desc;
      if (dirfd < 0)
	{
	  dirfd = openat (parentdirfd,
			  (parentdirfd < 0 ? dir->name
			   : last_component (dir->name)),
			  (O_RDONLY | O_DIRECTORY
			   | (no_dereference_symlinks ? O_NOFOLLOW : 0)));
	  if (dirfd < 0)
	    return false;
	  dir->desc = dirfd;
	}
      DIR *reading = fdopendir (dirfd);
      if (!reading)
        return false;
      dir->dirstream = reading;

      /* Initialize the table of filenames.  */

      idx_t data_alloc = 512;
      idx_t data_used = 0;
      dirdata->data = data = ximalloc (data_alloc);

      /* Read the directory entries, and insert the subfiles
         into the 'data' table.  */

      while (true)
        {
	  errno = 0;
	  struct dirent *next = readdir (reading);
	  if (!next)
	    break;

          char *d_name = next->d_name;
          idx_t d_size = _D_EXACT_NAMLEN (next) + 1;

          /* Ignore "." and "..".  */
          if (d_name[0] == '.'
              && (d_name[1] == 0 || (d_name[1] == '.' && d_name[2] == 0)))
            continue;

	  if (startfile)
	    {
	      int cmp = compare_names (d_name, startfile);
	      if (cmp < 0 || (startfile_only && !!cmp))
		continue;
	    }

          if (excluded_file_name (excluded, d_name))
            continue;

          if (data_alloc - data_used < d_size)
	    dirdata->data = data
	      = xpalloc (data, &data_alloc,
			 d_size - (data_alloc - data_used), -1, 1);
          memcpy (data + data_used, d_name, d_size);
          data_used += d_size;
          nnames++;
        }

      if (errno)
	return false;
    }

  /* Create the 'names' table from the 'data' table.  */
  char const **names = xinmalloc (nnames + 1, sizeof *names);
  dirdata->names = names;
  dirdata->nnames = nnames;
  for (idx_t i = 0; i < nnames; i++)
    {
      names[i] = data;
      data += strlen (data) + 1;
    }
  names[nnames] = nullptr;
  return true;
}

/* Compare strings in a locale-specific way, returning a value
   compatible with strcmp.  */

static int
compare_collated (char const *name1, char const *name2)
{
  errno = 0;
  int r = (ignore_file_name_case
	   ? strcasecoll (name1, name2)
	   : strcoll (name1, name2));
  if (errno)
    {
      error (0, errno, _("cannot compare file names '%s' and '%s'"),
             name1, name2);
      longjmp (failed_locale_specific_sorting, 1);
    }
  return r;
}

/* Compare file names, returning a value compatible with strcmp.  */

static int
compare_names (char const *name1, char const *name2)
{
  if (locale_specific_sorting)
    {
      int diff = compare_collated (name1, name2);
      if (diff || ignore_file_name_case)
        return diff;
    }
  return file_name_cmp (name1, name2);
}

/* Compare names FILE1 and FILE2 when sorting a directory.
   Prefer filtered comparison, breaking ties with file_name_cmp.  */

static int
compare_names_for_qsort (void const *file1, void const *file2)
{
  char const *const *f1 = file1;
  char const *const *f2 = file2;
  char const *name1 = *f1;
  char const *name2 = *f2;
  if (locale_specific_sorting)
    {
      int diff = compare_collated (name1, name2);
      if (diff)
        return diff;
    }
  return file_name_cmp (name1, name2);
}

/* Compare the contents of two directories named in CMP.
   This is a top-level routine; it does everything necessary for diff
   on two directories.

   If CMP->file[0].desc == NONEXISTENT, directory CMP->file[0] doesn't exist
   and pretend it is empty.  Otherwise, update CMP->file[0].desc and
   CMP->file[0].dirstream as needed.  Likewise for CMP->file[1].

   HANDLE_FILE is a caller-provided subroutine called to handle each file.
   It gets three operands: CMP, name of file in dir 0, name of file in dir 1.
   These names are relative to the original working directory.

   For a file that appears in only one of the dirs, one of the name-args
   to HANDLE_FILE is zero.

   Returns the maximum of all the values returned by HANDLE_FILE,
   or EXIT_TROUBLE if trouble is encountered in opening files.  */

int
diff_dirs (struct comparison *cmp,
           int (*handle_file) (struct comparison const *,
                               char const *, char const *))
{
  if ((cmp->file[0].desc == NONEXISTENT || dir_loop (cmp, 0))
      && (cmp->file[1].desc == NONEXISTENT || dir_loop (cmp, 1)))
    {
      error (0, 0, _("%s: recursive directory loop"),
             cmp->file[cmp->file[0].desc == NONEXISTENT].name);
      return EXIT_TROUBLE;
    }

  /* Get contents of both dirs.  */
  struct dirdata dirdata[2];
  int volatile val = EXIT_SUCCESS;
  for (int i = 0; i < 2; i++)
    if (! dir_read (cmp->parent->file[i].desc, &cmp->file[i], &dirdata[i],
		    cmp->parent == &noparent ? starting_file : nullptr, false))
      {
        perror_with_name (cmp->file[i].name);
        val = EXIT_TROUBLE;
      }

  if (val == EXIT_SUCCESS)
    {
      char const **volatile names[2] = {dirdata[0].names, dirdata[1].names};

      /* Use locale-specific sorting if possible, else native byte order.  */
      locale_specific_sorting = true;
      if (setjmp (failed_locale_specific_sorting))
        locale_specific_sorting = false;

      /* Sort the directories.  */
      for (int i = 0; i < 2; i++)
        qsort (names[i], dirdata[i].nnames, sizeof *dirdata[i].names,
               compare_names_for_qsort);

      /* Loop while files remain in one or both dirs.  */
      while (*names[0] || *names[1])
        {
          /* Compare next name in dir 0 with next name in dir 1.
             At the end of a dir,
             pretend the "next name" in that dir is very large.  */
          int nameorder = (!*names[0] ? 1 : !*names[1] ? -1
                           : compare_names (*names[0], *names[1]));

          /* Prefer a file_name_cmp match if available.  This algorithm is
             O(N**2), where N is the number of names in a directory
             that compare_names says are all equal, but in practice N
             is so small it's not worth tuning.  */
          if (nameorder == 0 && ignore_file_name_case)
            {
              int raw_order = file_name_cmp (*names[0], *names[1]);
              if (raw_order != 0)
                {
                  int greater_side = raw_order < 0;
                  int lesser_side = 1 - greater_side;
                  char const **lesser = names[lesser_side];
                  char const *greater_name = *names[greater_side];

                  for (char const **p = lesser + 1;
                       *p && compare_names (*p, greater_name) == 0;
                       p++)
                    {
                      int c = file_name_cmp (*p, greater_name);
                      if (0 <= c)
                        {
                          if (c == 0)
                            {
                              memmove (lesser + 1, lesser,
                                       (char *) p - (char *) lesser);
                              *lesser = greater_name;
                            }
                          break;
                        }
                    }
                }
            }

          int v1 = (*handle_file) (cmp,
                                   0 < nameorder ? 0 : *names[0]++,
                                   nameorder < 0 ? 0 : *names[1]++);
          if (val < v1)
            val = v1;
        }
    }

  for (int i = 0; i < 2; i++)
    {
      free (dirdata[i].names);
      free (dirdata[i].data);
    }

  return val;
}

/* Return nonzero if CMP is looping recursively in argument I.  */

static bool ATTRIBUTE_PURE
dir_loop (struct comparison const *cmp, int i)
{
  for (struct comparison const *p = cmp; (p = p->parent) != &noparent; )
    if (0 < same_file (&p->file[i].stat, &cmp->file[i].stat))
      return true;
  return false;
}

/* Find a matching filename in a directory.  */

char *
find_dir_file_pathname (struct file_data *dir, char const *file)
{
  char const *match = file;

  struct dirdata dirdata;
  dirdata.names = nullptr;
  dirdata.data = nullptr;

  if (ignore_file_name_case && dir_read (AT_FDCWD, dir, &dirdata, file, true))
    for (char const **p = dirdata.names; *p; p++)
      {
	if (file_name_cmp (*p, file) == 0)
	  {
	    match = *p;
	    break;
	  }
	if (match == file)
	  match = *p;
      }

  char *val = file_name_concat (dir->name, match, nullptr);
  free (dirdata.names);
  free (dirdata.data);
  return val;
}
