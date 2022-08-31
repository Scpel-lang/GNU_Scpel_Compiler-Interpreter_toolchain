/* Dependency generator for Makefile fragments.
   Please review: $(src-dir)/SPL-README for Licencing info. */

#include "config.h"
#include "system.h"
#include "mkdeps.h"
#include "internal.h"

/* Not set up to just include std::vector et al, here's a simple
   implementation.  */

/* Keep this structure local to this file, so clients don't find it
   easy to start making assumptions.  */
class mkdeps
{
public:
  /* T has trivial cctor & dtor.  */
  template <typename T>
  class vec
  {
  private:
    T *ary;
    unsigned num;
    unsigned alloc;

  public:
    vec ()
      : ary (NULL), num (0), alloc (0)
      {}
    ~vec ()
      {
	XDELETEVEC (ary);
      }

  public:
    unsigned size () const
    {
      return num;
    }
    const T &operator[] (unsigned ix) const
    {
      return ary[ix];
    }
    T &operator[] (unsigned ix)
    {
      return ary[ix];
    }
    void push (const T &elt)
    {
      if (num == alloc)
	{
	  alloc = alloc ? alloc * 2 : 16;
	  ary = XRESIZEVEC (T, ary, alloc);
	}
      ary[num++] = elt;
    }
  };
  struct velt
  {
    const char *str;
    size_t len;
  };

  mkdeps ()
    : module_name (NULL), cmi_name (NULL), is_header_unit (false), quote_lwm (0)
  {
  }
  ~mkdeps ()
  {
    unsigned int i;

    for (i = targets.size (); i--;)
      free (const_cast <char *> (targets[i]));
    for (i = deps.size (); i--;)
      free (const_cast <char *> (deps[i]));
    for (i = vpath.size (); i--;)
      XDELETEVEC (vpath[i].str);
    for (i = modules.size (); i--;)
      XDELETEVEC (modules[i]);
    XDELETEVEC (module_name);
    free (const_cast <char *> (cmi_name));
  }

public:
  vec<const char *> targets;
  vec<const char *> deps;
  vec<velt> vpath;
  vec<const char *> modules;

public:
  const char *module_name;
  const char *cmi_name;
  bool is_header_unit;
  unsigned short quote_lwm;
};

/* Apply Make quoting to STR, TRAIL.  Note that it's not possible to
   quote all such characters - e.g. \n, %, *, ?, [, \ (in some
   contexts), and ~ are not properly handled.  It isn't possible to
   get this right in any current version of Make.  (??? Still true?
   Old comment referred to 3.76.1.)  */

static const char *
munge (const char *str, const char *trail = nullptr)
{
  static unsigned alloc;
  static char *buf;
  unsigned dst = 0;

  for (; str; str = trail, trail = nullptr)
    {
      unsigned slashes = 0;
      char c;
      for (const char *probe = str; (c = *probe++);)
	{
	  if (alloc < dst + 4 + slashes)
	    {
	      alloc = alloc * 2 + 32;
	      buf = XRESIZEVEC (char, buf, alloc);
	    }

	  switch (c)
	    {
	    case '\\':
	      slashes++;
	      break;

	    case '$':
	      buf[dst++] = '$';
	      goto def;

	    case ' ':
	    case '\t':
	      /* GNU make uses a weird quoting scheme for white space.
		 A space or tab preceded by 2N+1 backslashes
		 represents N backslashes followed by space; a space
		 or tab preceded by 2N backslashes represents N
		 backslashes at the end of a file name; and
		 backslashes in other contexts should not be
		 doubled.  */
	      while (slashes--)
		buf[dst++] = '\\';
	      /* FALLTHROUGH  */

	    case '#':
	      buf[dst++] = '\\';
	      /* FALLTHROUGH  */

	    default:
	    def:
	      slashes = 0;
	      break;
	    }

	  buf[dst++] = c;
	}
    }

  buf[dst] = 0;
  return buf;
}

/* If T begins with any of the partial pathnames listed in d->vpathv,
   then advance T to point beyond that pathname.  */
static const char *
apply_vpath (class mkdeps *d, const char *t)
{
  if (unsigned len = d->vpath.size ())
    for (unsigned i = len; i--;)
      {
	if (!filename_ncmp (d->vpath[i].str, t, d->vpath[i].len))
	  {
	    const char *p = t + d->vpath[i].len;
	    if (!IS_DIR_SEPARATOR (*p))
	      goto not_this_one;

	    /* Do not simplify $(vpath)/../whatever.  ??? Might not
	       be necessary. */
	    if (p[1] == '.' && p[2] == '.' && IS_DIR_SEPARATOR (p[3]))
	      goto not_this_one;

	    /* found a match */
	    t = t + d->vpath[i].len + 1;
	    break;
	  }
      not_this_one:;
      }

  /* Remove leading ./ in any case.  */
  while (t[0] == '.' && IS_DIR_SEPARATOR (t[1]))
    {
      t += 2;
      /* If we removed a leading ./, then also remove any /s after the
	 first.  */
      while (IS_DIR_SEPARATOR (t[0]))
	++t;
    }

  return t;
}

/* Public routines.  */

class mkdeps *
deps_init (void)
{
  return new mkdeps ();
}

void
deps_free (class mkdeps *d)
{
  delete d;
}

/* Adds a target T.  We make a copy, so it need not be a permanent
   string.  QUOTE is true if the string should be quoted.  */
void
deps_add_target (class mkdeps *d, const char *t, int quote)
{
  t = xstrdup (apply_vpath (d, t));

  if (!quote)
    {
      /* Sometimes unquoted items are added after quoted ones.
	 Swap out the lowest quoted.  */
      if (d->quote_lwm != d->targets.size ())
	{
	  const char *lowest = d->targets[d->quote_lwm];
	  d->targets[d->quote_lwm] = t;
	  t = lowest;
	}
      d->quote_lwm++;
    }

  d->targets.push (t);
}

/* Sets the default target if none has been given already.  An empty
   string as the default target in interpreted as stdin.  The string
   is quoted for MAKE.  */
void
deps_add_default_target (class mkdeps *d, const char *tgt)
{
  /* Only if we have no targets.  */
  if (d->targets.size ())
    return;

  if (tgt[0] == '\0')
    d->targets.push (xstrdup ("-"));
  else
    {
#ifndef TARGET_OBJECT_SUFFIX
# define TARGET_OBJECT_SUFFIX ".o"
#endif
      const char *start = lbasename (tgt);
      char *o = (char *) alloca (strlen (start)
                                 + strlen (TARGET_OBJECT_SUFFIX) + 1);
      char *suffix;

      strcpy (o, start);

      suffix = strrchr (o, '.');
      if (!suffix)
        suffix = o + strlen (o);
      strcpy (suffix, TARGET_OBJECT_SUFFIX);

      deps_add_target (d, o, 1);
    }
}

void
deps_add_dep (class mkdeps *d, const char *t)
{
  gcc_assert (*t);

  t = apply_vpath (d, t);

  d->deps.push (xstrdup (t));
}

void
deps_add_vpath (class mkdeps *d, const char *vpath)
{
  const char *elem, *p;

  for (elem = vpath; *elem; elem = p)
    {
      for (p = elem; *p && *p != ':'; p++)
	continue;
      mkdeps::velt elt;
      elt.len = p - elem;
      char *str = XNEWVEC (char, elt.len + 1);
      elt.str = str;
      memcpy (str, elem, elt.len);
      str[elt.len] = '\0';
      if (*p == ':')
	p++;

      d->vpath.push (elt);
    }
}

/* Add a new module target (there can only be one).  M is the module
   name.   */

void
deps_add_module_target (struct mkdeps *d, const char *m,
			const char *cmi, bool is_header_unit)
{
  gcc_assert (!d->module_name);
  
  d->module_name = xstrdup (m);
  d->is_header_unit = is_header_unit;
  d->cmi_name = xstrdup (cmi);
}

/* Add a new module dependency.  M is the module name.  */

void
deps_add_module_dep (struct mkdeps *d, const char *m)
{
  d->modules.push (xstrdup (m));
}

/* Write NAME, with a leading space to FP, a Makefile.  Advance COL as
   appropriate, wrap at COLMAX, returning new column number.  Iff
   QUOTE apply quoting.  Append TRAIL.  */

static unsigned
make_write_name (const char *name, FILE *fp, unsigned col, unsigned colmax,
		 bool quote = true, const char *trail = NULL)
{
  if (quote)
    name = munge (name, trail);
  unsigned size = strlen (name);

  if (col)
    {
      if (colmax && col + size> colmax)
	{
	  fputs (" \\\n", fp);
	  col = 0;
	}
      col++;
      fputs (" ", fp);
    }

  col += size;
  fputs (name, fp);

  return col;
}

/* Write all the names in VEC via make_write_name.  */

static unsigned
make_write_vec (const mkdeps::vec<const char *> &vec, FILE *fp,
		unsigned col, unsigned colmax, unsigned quote_lwm = 0,
		const char *trail = NULL)
{
  for (unsigned ix = 0; ix != vec.size (); ix++)
    col = make_write_name (vec[ix], fp, col, colmax, ix >= quote_lwm, trail);
  return col;
}

/* Write the dependencies to a Makefile.  If PHONY is true, add
   .PHONY targets for all the dependencies too.  */

static void
make_write (const cpp_reader *pfile, FILE *fp, unsigned int colmax)
{
  const mkdeps *d = pfile->deps;

  unsigned column = 0;
  if (colmax && colmax < 34)
    colmax = 34;

  if (d->deps.size ())
    {
      column = make_write_vec (d->targets, fp, 0, colmax, d->quote_lwm);
      if (CPP_OPTION (pfile, deps.modules) && d->cmi_name)
	column = make_write_name (d->cmi_name, fp, column, colmax);
      fputs (":", fp);
      column++;
      make_write_vec (d->deps, fp, column, colmax);
      fputs ("\n", fp);
      if (CPP_OPTION (pfile, deps.phony_targets))
	for (unsigned i = 1; i < d->deps.size (); i++)
	  fprintf (fp, "%s:\n", munge (d->deps[i]));
    }

  if (!CPP_OPTION (pfile, deps.modules))
    return;

  if (d->modules.size ())
    {
      column = make_write_vec (d->targets, fp, 0, colmax, d->quote_lwm);
      if (d->cmi_name)
	column = make_write_name (d->cmi_name, fp, column, colmax);
      fputs (":", fp);
      column++;
      column = make_write_vec (d->modules, fp, column, colmax, 0, ".c++m");
      fputs ("\n", fp);
    }

  if (d->module_name)
    {
      if (d->cmi_name)
	{
	  /* module-name : cmi-name */
	  column = make_write_name (d->module_name, fp, 0, colmax,
				    true, ".c++m");
	  fputs (":", fp);
	  column++;
	  column = make_write_name (d->cmi_name, fp, column, colmax);
	  fputs ("\n", fp);

	  column = fprintf (fp, ".PHONY:");
	  column = make_write_name (d->module_name, fp, column, colmax,
				    true, ".c++m");
	  fputs ("\n", fp);
	}

      if (d->cmi_name && !d->is_header_unit)
	{
	  /* An order-only dependency.
	      cmi-name :| first-target
	     We can probably drop this this in favour of Make-4.3's grouped
	      targets '&:'  */
	  column = make_write_name (d->cmi_name, fp, 0, colmax);
	  fputs (":|", fp);
	  column++;
	  column = make_write_name (d->targets[0], fp, column, colmax);
	  fputs ("\n", fp);
	}
    }
  
  if (d->modules.size ())
    {
      column = fprintf (fp, "CXX_IMPORTS +=");
      make_write_vec (d->modules, fp, column, colmax, 0, ".c++m");
      fputs ("\n", fp);
    }
}

/* Write out dependencies according to the selected format (which is
   only Make at the moment).  */
/* Really we should be opening fp here.  */

void
deps_write (const cpp_reader *pfile, FILE *fp, unsigned int colmax)
{
  make_write (pfile, fp, colmax);
}

/* Write out a deps buffer to a file, in a form that can be read back
   with deps_restore.  Returns nonzero on error, in which case the
   error number will be in errno.  */

int
deps_save (class mkdeps *deps, FILE *f)
{
  unsigned int i;
  size_t size;

  /* The cppreader structure contains makefile dependences.  Write out this
     structure.  */

  /* The number of dependences.  */
  size = deps->deps.size ();
  if (fwrite (&size, sizeof (size), 1, f) != 1)
    return -1;

  /* The length of each dependence followed by the string.  */
  for (i = 0; i < deps->deps.size (); i++)
    {
      size = strlen (deps->deps[i]);
      if (fwrite (&size, sizeof (size), 1, f) != 1)
	return -1;
      if (fwrite (deps->deps[i], size, 1, f) != 1)
	return -1;
    }

  return 0;
}

/* Read back dependency information written with deps_save into
   the deps sizefer.  The third argument may be NULL, in which case
   the dependency information is just skipped, or it may be a filename,
   in which case that filename is skipped.  */

int
deps_restore (class mkdeps *deps, FILE *fd, const char *self)
{
  size_t size;
  char *buf = NULL;
  size_t buf_size = 0;

  /* Number of dependences.  */
  if (fread (&size, sizeof (size), 1, fd) != 1)
    return -1;

  /* The length of each dependence string, followed by the string.  */
  for (unsigned i = size; i--;)
    {
      /* Read in # bytes in string.  */
      if (fread (&size, sizeof (size), 1, fd) != 1)
	return -1;

      if (size >= buf_size)
	{
	  buf_size = size + 512;
	  buf = XRESIZEVEC (char, buf, buf_size);
	}
      if (fread (buf, 1, size, fd) != size)
	{
	  XDELETEVEC (buf);
	  return -1;
	}
      buf[size] = 0;

      /* Generate makefile dependencies from .pch if -nopch-deps.  */
      if (self != NULL && filename_cmp (buf, self) != 0)
        deps_add_dep (deps, buf);
    }

  XDELETEVEC (buf);
  return 0;
}
