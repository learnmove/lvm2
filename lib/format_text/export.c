/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "import-export.h"
#include "metadata.h"
#include "log.h"
#include "hash.h"
#include "pool.h"
#include "dbg_malloc.h"

#include <stdio.h>
#include <stdarg.h>
#include <time.h>


/*
 * The first half of this file deals with
 * exporting the vg, ie. writing it to a file.
 */
struct formatter {
	struct pool *mem;	/* pv names allocated from here */
	struct hash_table *pv_names; /* dev_name -> pv_name (eg, pv1) */

	FILE *fp;		/* where we're writing to */
	int indent;		/* current level of indentation */
};

#define MAX_INDENT 5
static void _inc_indent(struct formatter *f)
{
	if (++f->indent > MAX_INDENT)
		f->indent = MAX_INDENT;
}

static void _dec_indent(struct formatter *f)
{
	if (--f->indent) {
		log_debug("Indenting seems to have messed up\n");
		f->indent = 0;
	}
}

/*
 * Newline function for prettier layout.
 */
static void _nl(struct formatter *f)
{
	fprintf(f->fp, "\n");
}

#define COMMENT_TAB 6
static void _out_with_comment(struct formatter *f, const char *comment,
			      const char *fmt, va_list ap)
{
	int i;
	char white_space[MAX_INDENT + 1];

	for (i = 0; i < f->indent; i++)
		white_space[i] = '\t';
	white_space[i] = '\0';
	fprintf(f->fp, white_space);

	i = vfprintf(f->fp, fmt, ap);

	if (comment) {
		/*
		 * line comments up if possible.
		 */
		i += 8 * f->indent;
		i /= 8;
		i++;

		do
			fputc('\t', f->fp);

		while (++i < COMMENT_TAB);

		fprintf(f->fp, comment);
	}
	fputc('\n', f->fp);
}

/*
 * Formats a string, converting a size specified
 * in 512-byte sectors to a more human readable
 * form (eg, megabytes).  We may want to lift this
 * for other code to use.
 */
static int _sectors_to_units(uint64_t sectors, char *buffer, size_t s)
{
	static char *_units[] = {
		"Kilobytes",
		"Megabytes",
		"Gigabytes",
		"Terrabytes",
		NULL
		};

	int i;
	double d = (double) sectors;

	/* to convert to K */
	d /= 2.0;

	for (i = 0; (d > 1024.0)  && _units[i]; i++)
		d /= 1024.0;

	/* FIXME: arrange so this doesn't print a
	 * decimal point unless we have a
	 * fractional part. */
	return snprintf(buffer, s, "# %g %s", d, _units[i]) > 0;
}

/*
 * Appends a comment giving a size in more easily
 * readable form (eg, 4M instead of 8096).
 */
static void _out_size(struct formatter *f, uint64_t size,
		      const char *fmt, ...)
{
	char buffer[64];
	va_list ap;

	_sectors_to_units(size, buffer, sizeof(buffer));

	va_start(ap, fmt);
	_out_with_comment(f, buffer, fmt, ap);
	va_end(ap);
}

/*
 * Appends a comment indicating that the line is
 * only a hint.
 */
static void _out_hint(struct formatter *f, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	_out_with_comment(f, "# Hint only", fmt, ap);
	va_end(ap);
}

/*
 * The normal output function.
 */
static void _out(struct formatter *f, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	_out_with_comment(f, NULL, fmt, ap);
	va_end(ap);
}

static int _print_header(struct formatter *f, struct volume_group *vg)
{
	time_t t;

	t = time(NULL);

	_out(f,
	     "# This file was originally generated by the LVM2 library\n"
	     "# It is inadvisable for people to edit this by hand unless\n"
	     "# they *really* know what they're doing.\n"
	     "# Generated: %s\n", ctime(&t));
	return 1;
}

static int _print_vg(struct formatter *f, struct volume_group *vg)
{
	char buffer[256];

	if (!id_write_format(&vg->id, buffer, sizeof(buffer))) {
		stack;
		return 0;
	}

	_out(f, "id = \"%s\"", buffer);
	_nl(f);

	if (!print_flags(vg->status, VG_FLAGS, buffer, sizeof(buffer))) {
		stack;
		return 0;
	}

	_out(f, "status = %s");
	_nl(f);

	_out_size(f, vg->extent_size, "extent_size = %u", vg->extent_size);
	_nl(f);

	_out(f, "max_lv = %u", vg->max_lv);
	_out(f, "max_pv = %u", vg->max_pv);
	_nl(f);

	return 1;
}

/*
 * Get the pv%d name from the formatters hash
 * table.
 */
static inline const char *
_get_pv_name(struct formatter *f, struct physical_volume *pv)
{
	return (const char *) hash_lookup(f->pv_names, dev_name(pv->dev));
}

static int _print_pvs(struct formatter *f, struct volume_group *vg)
{
	struct list *pvh;
	struct physical_volume *pv;
	char buffer[256];
	const char *name;

	_out(f, "physical_volumes {");
	_nl(f);
	_inc_indent(f);

	list_iterate (pvh, &vg->pvs) {

		if (!(name = _get_pv_name(f, pv))) {
			stack;
			return 0;
		}

		_out(f, "%s {", name);
		_inc_indent(f);

		pv = &list_item(pvh, struct pv_list)->pv;

		if (!id_write_format(&pv->id, buffer, sizeof(buffer))) {
			stack;
			return 0;
		}

		_out(f, "tid = \"%s\"", buffer);
		_out_hint(f, "device = %s", dev_name(pv->dev));
		_nl(f);

		if (!print_flags(pv->status, PV_FLAGS,
				 buffer, sizeof(buffer))) {
			stack;
			return 0;
		}

		_out(f, "status = %s", buffer);
		_out(f, "pe_start = %u", pv->pe_start);
		_out_size(f, vg->extent_size * (uint64_t) pv->pe_count,
			  "pe_count = %u", pv->pe_count);

		_dec_indent(f);
		_out(f, "}");
		_nl(f);
	}

	_dec_indent(f);
	_out(f, "}");
	return 1;
}

static int _print_segment(struct formatter *f, struct volume_group *vg,
			  int count, struct stripe_segment *seg, int last)
{
	int s;
	const char *name;

	_out(f, "segment%u {", count);
	_inc_indent(f);

	_out(f, "start_entent = %u", seg->le);
	_out_size(f, seg->len * vg->extent_size, "extent_count = %u", seg->len);
	_out(f, "stripes = %u", seg->stripes);

	if (seg->stripes > 1)
		_out_size(f, seg->stripe_size,
			  "stripe_size = %u", seg->stripe_size);

	_nl(f);
	_out(f, "areas = [");
	_inc_indent(f);

	for (s = 0; s < seg->stripes; s++) {
		if (!(name = _get_pv_name(f, seg->area[s].pv))) {
			stack;
			return 0;
		}

		_out(f, "\"%s\", %u%s", name, seg->area[s].pe,
		     last ? "" : ",");
	}

	_dec_indent(f);
	_out(f, "]");

	_dec_indent(f);
	_out(f, "}");

	return 1;
}

static int _count_segments(struct logical_volume *lv)
{
	int r = 0;
	struct list *segh;

	list_iterate (segh, &lv->segments)
		r++;

	return r;
}

static int _print_lvs(struct formatter *f, struct volume_group *vg)
{
	struct list *lvh, *segh;
	struct logical_volume *lv;
	struct stripe_segment *seg;
	char buffer[256];
	int seg_count = 1;

	_out(f, "logical_volumes {");
	_nl(f);
	_inc_indent(f);

	list_iterate (lvh, &vg->lvs) {
		lv = &list_item(lvh, struct lv_list)->lv;

		_out(f, "%s {", lv->name);
		_inc_indent(f);

		if (!print_flags(lv->status, LV_FLAGS,
				 buffer, sizeof(buffer))) {
			stack;
			return 0;
		}

		_out(f, "read_ahead = %u", lv->read_ahead);
		_nl(f);

		_out(f, "segment_count = %u", _count_segments(lv));
		_nl(f);

		list_iterate (segh, &lv->segments) {
			seg = list_item(segh, struct stripe_segment);

			if (!_print_segment(f, vg, seg_count++, seg,
				    (segh->n == &lv->segments))) {
				stack;
				return 0;
			}
		}

		_out(f, "}");
		_dec_indent(f);
	}

	_out(f, "}");
	_nl(f);
	_dec_indent(f);

	return 1;
}

/*
 * In the text format we refer to pv's as 'pv1',
 * 'pv2' etc.  This function builds a hash table
 * to enable a quick lookup from device -> name.
 */
static int _build_pv_names(struct formatter *f,
			   struct volume_group *vg)
{
	int count = 0;
	struct list *pvh;
	struct physical_volume *pv;
	char buffer[32], *name;

	if (!(f->mem = pool_create(512))) {
		stack;
		goto bad;
	}

	if (!(f->pv_names = hash_create(128))) {
		stack;
		goto bad;
	}

	list_iterate (pvh, &vg->pvs) {
		pv = &list_item(pvh, struct pv_list)->pv;

		if (snprintf(buffer, sizeof(buffer), "pv%d", count++) < 0) {
			stack;
			goto bad;
		}

		if (!(name = pool_strdup(f->mem, buffer))) {
			stack;
			goto bad;
		}

		if (!hash_insert(f->pv_names, dev_name(pv->dev), buffer)) {
			stack;
			goto bad;
		}
	}

	return 1;

 bad:
	if (f->mem)
		pool_destroy(f->mem);

	if (f->pv_names)
		hash_destroy(f->pv_names);

	return 0;
}

int text_vg_export(FILE *fp, struct volume_group *vg)
{
	int r = 0;
	struct formatter *f;

	if (!(f = dbg_malloc(sizeof(*f)))) {
		stack;
		return 0;
	}

	memset(f, 0, sizeof(*f));
	f->fp = fp;
	f->indent = 0;

	if (!_build_pv_names(f, vg)) {
		stack;
		goto out;
	}

#define fail do {stack; goto out;} while(0)

	if (!_print_header(f, vg))
		fail;

	_out(f, "%s {", vg->name);
	_inc_indent(f);

	if (!_print_vg(f, vg))
		fail;

	if (!_print_pvs(f, vg))
		fail;

	if (!_print_lvs(f, vg))
		fail;

#undef fail

	_dec_indent(f);
	_out(f, "}");

	r = 1;

 out:
	if (f->mem)
		pool_destroy(f->mem);

	if (f->pv_names)
		hash_destroy(f->pv_names);

	dbg_free(f);
	return r;
}
