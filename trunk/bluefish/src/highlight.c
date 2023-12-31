/* Bluefish HTML Editor
 * highlight.c - the syntax highlighting
 *
 * Copyright (C) 2002 Olivier Sessink
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *
 * indenting is done with
 * indent --line-length 100 --k-and-r-style --tab-size 4 -bbo --ignore-newlines highlight.c
 */
#define HL_TIMING
/* #define HL_DEBUG */

#ifdef HL_TIMING
#include <sys/times.h>
#include <unistd.h>
#endif

#include <gtk/gtk.h>
#include <sys/types.h>			/* _before_ regex.h for freeBSD */
#include <regex.h>				/* regexec() */
#include <string.h>				/* strerror() */
#include <stdlib.h>				/* atoi() */

#include "bluefish.h"
#include "bf_lib.h"				/* filename_test_extensions() */
#include "rcfile.h"				/* array_from_arglist() */
#include "stringlist.h" 	/* count_array() */
#include "menu.h" 			/* menu_current_document_type_set_active_wo_activate */
#include "highlight.h"

#define NUM_SUBMATCHES 20

typedef struct {
	regmatch_t pmatch[NUM_SUBMATCHES];
	gint pmatch_offset;
	gboolean is_match;
	Tpattern *pat;
} Tpatmatch;

typedef struct {
	GtkTextTagTable *tagtable; /* this one should ultimately move to Tfiletype, so every set would have it's own tagtable, but there is currently no way to switch a document to a new tagtable */
	GList *highlight_filetypes;
} Thighlight;

/*********************************/
/* global vars for this module   */
/*********************************/
static Thighlight highlight;

/*********************************/
/* initializing the highlighting */
/*********************************/

static void pattern_free(Tpattern * pat)
{
	g_free(pat->name);
	g_free(pat->spat1);
	regfree(&pat->rpat1);
	if (pat->spat2) {
		g_free(pat->spat2);
		regfree(&pat->rpat2);
	}
	if (pat->spat3) {
		g_free(pat->spat3);
		regfree(&pat->rpat3);
	}
	g_free(pat);
}

static gboolean add_pattern_to_name(GList * patternlist, regex_t * parentmatch, Tpattern * pat)
{
	Tpattern *parpat;
	gboolean resval = FALSE;
	GList *tmplist;

#ifdef DEBUG
	g_assert(pat);
	g_assert(patternlist);
#endif
	tmplist = g_list_first(patternlist);
	while (tmplist) {
		if (tmplist->data != pat) {
			if (((Tpattern *) tmplist->data)->childs) {
				gboolean tmpresval;

				tmpresval = add_pattern_to_name(((Tpattern *) tmplist->data)->childs, parentmatch, pat);
				if (!resval && tmpresval)
					resval = TRUE;
			}
			if (regexec(parentmatch, (gchar *) ((Tpattern *) tmplist->data)->name, 0, NULL, 0) == 0) {
				parpat = (Tpattern *) tmplist->data;
				parpat->childs = g_list_append(parpat->childs, pat);
				resval = TRUE;
				if (pat->mode == 3) {
					parpat->need_subpatterns = TRUE;
				}
			}
		}
		tmplist = g_list_next(tmplist);
	}
	return resval;
}

static void hl_compile_pattern(Tfiletype * hlset, gchar * name, gint case_insens, gchar * pat1, gchar * pat2,
							   gchar * pat3, gint mode, gchar * parent, gchar * fgcolor, gchar * bgcolor, gint weight,
							   gint style, gchar *start_boundary, gchar *end_boundary)
{
	/*
	 * test if the pattern is correct 
	 */
	DEBUG_MSG("compile_patterns, testing name=%s\n", name);
	if (!name || strlen(name) == 0) {
		g_print("error compiling nameless pattern: name is not set\n");
	}
	switch (mode) {
	case 0:
		if (!(pat1 && pat2 && pat3 && strlen(pat1) && strlen(pat2)
			  && strlen(pat3))) {
			g_print("error compiling pattern %s for mode 0: some pattern(s) missing\n", name);
			return;
		}
		break;
	case 1:
		if (!(pat1 && pat2 && strlen(pat1) && strlen(pat2))) {
			g_print("error compiling pattern %s for mode 1: some pattern(s) missing\n", name);
			return;
		}
		break;
	case 2:
		if (!(pat1 && strlen(pat1))) {
			g_print("error compiling pattern %s for mode 2: pattern missing\n", name);
			return;
		}
		break;
	case 3:
		if (!(pat1 && strlen(pat1) && atoi(pat1) > 0 && atoi(pat1) < NUM_SUBMATCHES)) {
			g_print("error compiling pattern %s for mode 3: sub-pattern number missing or incorrect\n", name);
			return;
		}
		break;
	default:
		g_print("error compiling pattern %s: mode %d unknown\n", name, mode);
		return;
		break;
	}

	{
		Tpattern *pat;
		gint res;

		DEBUG_MSG("compile_patterns, compiling name=%s\n", name);

		pat = g_malloc0(sizeof(Tpattern));
		switch (mode) {
		case 0:
			pat->spat3 = g_strdup(pat3);
		case 1:
			pat->spat2 = g_strdup(pat2);
		case 2:
		case 3:
			pat->spat1 = g_strdup(pat1);
		default:
			pat->name = g_strdup(name);
			pat->mode = mode;
			DEBUG_MSG("compile_patterns, done copying\n");
			break;
		}
		switch (mode) {
		case 0:
			if (!case_insens) {
				res = regcomp(&pat->rpat3, pat3, REG_EXTENDED);
			} else {
				res = regcomp(&pat->rpat3, pat3, REG_EXTENDED | REG_ICASE);
			}
			if (0 != res) {
				g_print("error compiling 3rd pattern %s: %s because %s\n", name, pat3, strerror(res));
				g_free(pat->spat1);
				g_free(pat->spat2);
				g_free(pat->spat3);
				g_free(pat->name);
				g_free(pat);
				return;
			}
		case 1:
			if (!case_insens) {
				res = regcomp(&pat->rpat2, pat2, REG_EXTENDED);
			} else {
				res = regcomp(&pat->rpat2, pat2, REG_EXTENDED | REG_ICASE);
			}
			if (0 != res) {
				g_print("error compiling 2nd pattern %s: %s because %s\n", name, pat2, strerror(res));
				if (mode == 0) {
					g_free(pat->spat3);
					regfree(&pat->rpat3);
				}
				g_free(pat->spat1);
				g_free(pat->spat2);
				g_free(pat->name);
				g_free(pat);
				return;
			}
		case 2:
			if (!case_insens) {
				res = regcomp(&pat->rpat1, pat1, REG_EXTENDED);
			} else {
				res = regcomp(&pat->rpat1, pat1, REG_EXTENDED | REG_ICASE);
			}
			if (0 != res) {
				g_print("error compiling 1st pattern %s: %s because %s\\n", name, pat1, strerror(res));
				switch (mode) {
				case 0:
					regfree(&pat->rpat3);
					g_free(pat->spat3);
				case 1:
					regfree(&pat->rpat2);
					g_free(pat->spat2);
				default:
					break;
				}
				g_free(pat->spat1);
				g_free(pat->name);
				g_free(pat);
				return;
			}
			break;
		case 3:
			{
				gint num;

				num = atoi(pat1);
				if (num <= 0 || num > NUM_SUBMATCHES) {
					g_free(pat->spat1);
					g_free(pat->name);
					g_free(pat);
					return;
				}
			}
			break;
		default:
			break;
		}
		if (start_boundary && strlen(start_boundary)) {
			pat->start_boundary = g_strdup(start_boundary);
		} else {
			pat->start_boundary = NULL;
		}
		if (end_boundary && strlen(end_boundary)) {
			pat->end_boundary = g_strdup(end_boundary);
		} else {
			pat->end_boundary = NULL;
		}
		
		pat->tag = gtk_text_tag_new(NULL);

		if (strlen(fgcolor)) {
			g_object_set(pat->tag, "foreground", fgcolor, NULL);
		}
		if (strlen(bgcolor)) {
			g_object_set(pat->tag, "background", bgcolor, NULL);
		}
		if (weight > 0) {
			if (1 == weight) {
				g_object_set(pat->tag, "weight", PANGO_WEIGHT_NORMAL, NULL);
			} else {
				g_object_set(pat->tag, "weight", PANGO_WEIGHT_BOLD, NULL);
			}
		}
		if (style > 0) {
			if (1 == style) {
				g_object_set(pat->tag, "style", PANGO_STYLE_NORMAL, NULL);
			} else {
				g_object_set(pat->tag, "style", PANGO_STYLE_ITALIC, NULL);
			}
		}
		gtk_text_tag_table_add(highlight.tagtable, pat->tag);
		/*
		 * now add the pattern to the hierarchy 
		 */
		{
			regex_t parentmatch;
			gboolean retval1 = FALSE, retval2;
			if (parent && strlen(parent)) {
				regcomp(&parentmatch, parent, REG_EXTENDED | REG_NOSUB);
			} else {
				regcomp(&parentmatch, "^top$", REG_EXTENDED | REG_NOSUB);
			}
			if (regexec(&parentmatch, "top", 0, NULL, 0) == 0) {
				hlset->highlightlist = g_list_append(hlset->highlightlist, pat);
				retval1 = TRUE;
			}
			retval2 = add_pattern_to_name(hlset->highlightlist, &parentmatch, pat);
			if (!retval1 && !retval2) {
				DEBUG_MSG("could NOT add child %s to parent %s\n", name, parent);
				pattern_free(pat);
			}
			regfree(&parentmatch);
		}

	}
}


Tfiletype *hl_get_highlightset_by_type(gchar * type)
{
	GList *tmplist;
	tmplist = g_list_first(main_v->filetypelist);
	while (tmplist) {
#ifdef DEBUG
		if (!tmplist || !tmplist->data || !((Tfiletype *) tmplist->data)->type) {
			DEBUG_MSG("hl_get_highlightset_by_type is perhaps not yet finished??\n");
			exit(1);
		}
#endif							/* DEBUG */
		if (strcmp(((Tfiletype *) tmplist->data)->type, type) == 0) {
			return (Tfiletype *) tmplist->data;
		}
		tmplist = g_list_next(tmplist);
	}
	return NULL;
}


void hl_init()
{
	GList *tmplist;
	/* init main_v->filetypelist, the first set is the defaultset */

	highlight.highlight_filetypes = NULL;
	highlight.tagtable = gtk_text_tag_table_new();

	/* start by initializing the types, they should come from the configfile */
	tmplist = g_list_first(main_v->props.filetypes);
	while (tmplist) {
		gchar **strarr;
		Tfiletype *filetype;
		strarr = (gchar **) tmplist->data;
		if (count_array(strarr) == 4) {
			filetype = g_new(Tfiletype, 1);
			filetype->type = g_strdup(strarr[0]);
			filetype->extensions = g_strsplit(strarr[1], ":", 127);
			filetype->update_chars = g_strdup(strarr[2]);
			if (strlen(strarr[3])){
				GError *error=NULL;
				filetype->icon = gdk_pixbuf_new_from_file(strarr[3], &error);
				if (error != NULL) {
					/* Report error to user, and free error */
					g_print("ERROR while loading icon: %s\n", error->message);
					g_error_free(error);
					filetype->icon = NULL;
				}
			} else {
				filetype->icon = NULL;
			}
			filetype->highlightlist = NULL;
			main_v->filetypelist = g_list_append(main_v->filetypelist, filetype);
		}
#ifdef DEBUG
		else {
			DEBUG_MSG("hl_init, filetype needs 4 params in array\n");
		}
#endif
		tmplist = g_list_next(tmplist);
	}

	/* now start adding all the patterns */

	tmplist = g_list_first(main_v->props.highlight_patterns);
	while (tmplist) {
		Tfiletype *hlset;
		gchar **strarr;
		strarr = (gchar **) tmplist->data;

		if (count_array(strarr) == 14) {
			hlset = hl_get_highlightset_by_type(strarr[0]);
			hl_compile_pattern(hlset, strarr[1], atoi(strarr[2]), strarr[3]
							   , strarr[4], strarr[5], atoi(strarr[6]), strarr[7], strarr[8]
							   , strarr[9], atoi(strarr[10]), atoi(strarr[11]), strarr[12], strarr[13]);
		}
#ifdef DEBUG
		else {
			DEBUG_MSG("hl_init, hl pattern needs 14 params in array, this one has %d\n", count_array(strarr));
		}
#endif
		tmplist = g_list_next(tmplist);
	}
	
	/* now link the types with highlight patterns from a new list*/
	tmplist = g_list_first(main_v->filetypelist);
	while (tmplist) {
		if (((Tfiletype *)tmplist->data)->highlightlist) {
			highlight.highlight_filetypes = g_list_append(highlight.highlight_filetypes, tmplist->data);
		}
		tmplist = g_list_next(tmplist);
	}
}

Tfiletype *hl_get_highlightset_by_filename(gchar * filename)
{
	GList *tmplist;
	/* find set for this filetype */
	if (filename) {
		tmplist = g_list_first(highlight.highlight_filetypes);
		while (tmplist) {
			if (filename_test_extensions(((Tfiletype *) tmplist->data)->extensions, filename)) {
				return (Tfiletype *) tmplist->data;
			}
			tmplist = g_list_next(tmplist);
		}
	}
	/* if none found return first set (is default set) */
	tmplist = g_list_first(highlight.highlight_filetypes);
	if (!tmplist) {
		DEBUG_MSG("hl_get_highlightset_by_filename, no highlightsets?\n");
		return NULL;
	} else {
		return (Tfiletype *) tmplist->data;
	}
}

/*****************************/
/* applying the highlighting */
/*****************************/

static void patmatch_rematch(Tpatmatch *patmatch, gint offset, regmatch_t par_pmatch[], gchar *string) {
	gboolean done = FALSE;
	gint localoffset=0;
#ifdef HL_DEBUG
	g_print("patmatch(%p), offset=%d, string(%p)\n", patmatch, offset, string);
	if (offset < 0) {
		g_print("patmatch_rematch, offset=%d, ABORTING\n", offset);
		exit(2);
	}
	if (offset > strlen(string)) {
		g_print("patmatch_rematch, offset=%d, strlen(string)=%d, ABORTING\n", offset, strlen(string));
		exit(3);
	}
#endif

	while (!done) {
		if (patmatch->pat->mode == 3) {
			gint num = atoi(patmatch->pat->spat1);
			patmatch->pmatch_offset = 0;
			patmatch->pmatch[0].rm_so = par_pmatch[num].rm_so;
			patmatch->pmatch[0].rm_eo = par_pmatch[num].rm_eo;
			patmatch->pmatch[1].rm_so = -1;
			done = TRUE;
		} else {
			gint res, nummatches;
			nummatches = (patmatch->pat->need_subpatterns) ? NUM_SUBMATCHES : 1;
			res = regexec(&patmatch->pat->rpat1, &string[offset+localoffset], nummatches, patmatch->pmatch, 0);
			if (res != 0) {
				patmatch->pmatch[0].rm_so = -1;
				done = TRUE;
			} else {
				gboolean start_ok = TRUE, end_ok = TRUE;
#ifdef HL_DEBUG
				gchar *debugstr = g_strndup(&string[offset+localoffset+patmatch->pmatch[0].rm_so-1], (patmatch->pmatch[0].rm_eo - patmatch->pmatch[0].rm_so + 2));
				g_print("patmatch_rematch, '%s', rm_so=%d, rm_eo=%d, offset=%d, localoffset=%d\n", debugstr, patmatch->pmatch[0].rm_so, patmatch->pmatch[0].rm_eo, offset, localoffset);
				g_free(debugstr);
#endif
				if (patmatch->pat->start_boundary && ((patmatch->pmatch[0].rm_so + localoffset + offset) > 0)) { /* a start at position 0 does not have any characters in front */
					gchar c = string[offset+localoffset+patmatch->pmatch[0].rm_so-1];
					start_ok = (strchr(patmatch->pat->start_boundary, c) != NULL);
#ifdef HL_DEBUG
					g_print("patmatch_rematch, start_boundary='%c', test in '%s', result=%d\n", c, patmatch->pat->start_boundary, start_ok);
#endif
				}
				if (patmatch->pat->end_boundary && patmatch->pat->mode == 2) {
					gchar c = string[offset+localoffset+patmatch->pmatch[0].rm_eo];
#ifdef HL_DEBUG
					g_print("patmatch_rematch, end_boundary='%c' (at %d), test in '%s', result=%d\n", c, offset+localoffset+patmatch->pmatch[0].rm_eo+1,patmatch->pat->end_boundary, start_ok);
#endif
					end_ok = (strchr(patmatch->pat->end_boundary, c) != NULL);
				}
				if (start_ok && end_ok) {
					done = TRUE;
#ifdef HL_DEBUG
					g_print("patmatch_rematch, start_ok&&end_ok, setting pmatch_offset to offset(%d)+localoffset(%d)=%d\n",offset, localoffset, offset+localoffset);
#endif
					patmatch->pmatch_offset = offset+localoffset;
				} else {
					/* we have to increase the offset for this pattern to the found start + 1 */
#ifdef HL_DEBUG
					g_print("patmatch_rematch, setting localoffset from %d to %d\n", localoffset, (localoffset + patmatch->pmatch[0].rm_so+1));
#endif
					localoffset += patmatch->pmatch[0].rm_so+1;
				}
			}
		}
	}
#ifdef HL_DEBUG
	g_print("patmatch_rematch, patmatch %s now starts at %d (offset=%d)\n", patmatch->pat->name,patmatch->pmatch[0].rm_so, patmatch->pmatch_offset);
#endif
}

static void applylevel(Tdocument * doc, GList * level_list, gint start, gint end, regmatch_t par_pmatch[], gchar * buf)
{
#ifdef HL_DEBUG
	DEBUG_MSG("applylevel, started on doc %p from %d to %d\n", doc, start, end);
	if (buf) {
		if (strlen(buf) < 50) {
			DEBUG_MSG("applylevel, buf='%s'\n", buf);
		} else {
			DEBUG_MSG("applylevel, strlen(buf)='%d'\n", strlen(buf));
		}
	}
#endif
	if (!level_list || (start == end)) {
		DEBUG_MSG("applylevel, no list or start==end\n");
		return;
	} else {
		gchar *string;
		gint offset = 0;
		Tpatmatch *patmatch;
		gint numpats;
		/* first get the buffer we will work on this level */
		if (buf) {
			string = buf;
		} else {
			GtkTextIter itstart, itend;

			gtk_text_buffer_get_iter_at_offset(doc->buffer, &itstart, start);
			gtk_text_buffer_get_iter_at_offset(doc->buffer, &itend, end);

			string = gtk_text_buffer_get_text(doc->buffer, &itstart, &itend, FALSE);
#ifdef HL_DEBUG
			DEBUG_MSG("len van string =%d\n", strlen(string));
#endif
		}
		{
			GList *tmplist;
			gint i = 0;
			numpats = g_list_length(level_list);
			patmatch = g_new(Tpatmatch, numpats + 1);
			tmplist = g_list_first(level_list);
			while (tmplist) {
				patmatch[i].pat = (Tpattern *) tmplist->data;
				tmplist = g_list_next(tmplist);
				i++;
			}
		}
		/* search for the first match on all patterns */
		{
			gint i;
			for (i = 0; i < numpats; i++) {
				patmatch_rematch(&patmatch[i], offset, par_pmatch, string);
			}
		}

		/* loop trough the text */
		while (TRUE) {			/* the break is done automatically when lowest_match_pat == NULL */
			gint lowest_patmatch = -1;
			{
				gint i, lowest = end - start + 1;
				for (i = 0; i < numpats; i++) {
					if (patmatch[i].pmatch[0].rm_so >= 0
						&& (patmatch[i].pmatch[0].rm_so + patmatch[i].pmatch_offset) < lowest) {
						lowest_patmatch = i;
						lowest = patmatch[i].pmatch[0].rm_so + patmatch[i].pmatch_offset;
					}
				}
			}
			if (lowest_patmatch == -1) {
#ifdef HL_DEBUG
				DEBUG_MSG("applylevel, no lowest match anymore on this level, return!!\n");
#endif
				break;
			}
#ifdef HL_DEBUG
			DEBUG_MSG("applylevel, pat(%d) %s has the lowest start %d (so=%d+offset=%d)\n", lowest_patmatch,
					  patmatch[lowest_patmatch].pat->name,
					  patmatch[lowest_patmatch].pmatch[0].rm_so + patmatch[lowest_patmatch].pmatch_offset,
					  patmatch[lowest_patmatch].pmatch[0].rm_so, patmatch[lowest_patmatch].pmatch_offset);
#endif
			/* apply this match, if we can't find a ending match or so, we just set
			 * the start to -1 so we will not use it anymore, and we set the start 
			 * back to the current start */
			if (patmatch[lowest_patmatch].pat->mode == 0 || patmatch[lowest_patmatch].pat->mode == 1) {
					regmatch_t pmatch[1];
					gint res2;
#ifdef HL_DEBUG
					DEBUG_MSG("applylevel, %s has mode==1, we need a 2nd match starting at %d\n", patmatch[lowest_patmatch].pat->name,
							  patmatch[lowest_patmatch].pmatch[0].rm_so + patmatch[lowest_patmatch].pmatch_offset + 1);
#endif
					res2=regexec(&patmatch[lowest_patmatch].pat->rpat2,&string[patmatch[lowest_patmatch].pmatch[0].rm_so + patmatch[lowest_patmatch].pmatch_offset +1], 1, pmatch, 0);
					if (res2 == 0) {
						gboolean boundary_ok = TRUE;
						if (patmatch[lowest_patmatch].pat->end_boundary) {
							gchar c = string[pmatch[0].rm_eo + patmatch[lowest_patmatch].pmatch[0].rm_so + patmatch[lowest_patmatch].pmatch_offset +1];
#ifdef HL_DEBUG
							g_print("applylevel, end_boundary='%c' (at %d), test in '%s', result=%d\n", c,pmatch[0].rm_eo + patmatch[lowest_patmatch].pmatch[0].rm_so + patmatch[lowest_patmatch].pmatch_offset +1 ,patmatch[lowest_patmatch].pat->end_boundary, boundary_ok);
#endif
							if(c != '\0' && strchr(patmatch[lowest_patmatch].pat->end_boundary, c) == NULL) {
								boundary_ok = FALSE;
							}
						}
						if (patmatch[lowest_patmatch].pat->mode == 0) {
							/* invalidating a mode 0 pattern should go here */
							g_print("the pattern invalidating part for mode 0 is not yet implemented\n");
						}
						
						if (boundary_ok) {
							patmatch[lowest_patmatch].pmatch[0].rm_eo =
									pmatch[0].rm_eo + patmatch[lowest_patmatch].pmatch[0].rm_so + 1;
#ifdef HL_DEBUG
							DEBUG_MSG("found match at %d, adding offset in string minus pmatch_offset %d to that\n",
									  pmatch[0].rm_eo, patmatch[lowest_patmatch].pmatch[0].rm_so + 1);
#endif
							patmatch[lowest_patmatch].is_match = TRUE;
						} else {
							patmatch[lowest_patmatch].is_match = FALSE;
						}
					} else {
						patmatch[lowest_patmatch].is_match = FALSE;
					}
				} else {
					patmatch[lowest_patmatch].is_match = TRUE;
				}
			if (patmatch[lowest_patmatch].is_match) {
				GtkTextIter itstart, itend;
				gint istart, iend;
				glong char_start, char_end, byte_char_diff_start;
				char_start = utf8_byteoffset_to_charsoffset(string, patmatch[lowest_patmatch].pmatch[0].rm_so + patmatch[lowest_patmatch].pmatch_offset);
				char_end = utf8_byteoffset_to_charsoffset(string, patmatch[lowest_patmatch].pmatch[0].rm_eo + patmatch[lowest_patmatch].pmatch_offset);
				byte_char_diff_start = (patmatch[lowest_patmatch].pmatch[0].rm_so + patmatch[lowest_patmatch].pmatch_offset)-char_start;
				
/*				istart = start + patmatch[lowest_patmatch].pmatch[0].rm_so + patmatch[lowest_patmatch].pmatch_offset;
				iend = start + patmatch[lowest_patmatch].pmatch[0].rm_eo + patmatch[lowest_patmatch].pmatch_offset;*/
				istart = start + char_start;
				iend = start + char_end;
#ifdef HL_DEBUG
				DEBUG_MSG("applylevel, byte_char_diff=%d\n", byte_char_diff_start);
				DEBUG_MSG("applylevel, coloring from %d to %d\n", istart, iend);
#endif
				gtk_text_buffer_get_iter_at_offset(doc->buffer, &itstart, istart);
				gtk_text_buffer_get_iter_at_offset(doc->buffer, &itend, iend);
				gtk_text_buffer_apply_tag(doc->buffer, patmatch[lowest_patmatch].pat->tag, &itstart, &itend);
				if (patmatch[lowest_patmatch].pat->childs) {
					if (patmatch[lowest_patmatch].pat->need_subpatterns) {
						gint i = 1;
						while (patmatch[lowest_patmatch].pmatch[i].rm_so > 0) {
#ifdef HL_DEBUG
							DEBUG_MSG("before: subpattern i=%d has so=%d and eo=%d\n", i,
									  patmatch[lowest_patmatch].pmatch[i].rm_so, patmatch[lowest_patmatch].pmatch[i].rm_eo);
#endif
							patmatch[lowest_patmatch].pmatch[i].rm_so -= (patmatch[lowest_patmatch].pmatch[0].rm_so);
							patmatch[lowest_patmatch].pmatch[i].rm_eo -= (patmatch[lowest_patmatch].pmatch[0].rm_so);
#ifdef HL_DEBUG
							DEBUG_MSG("after: subpattern i=%d has so=%d and eo=%d\n", i,
									  patmatch[lowest_patmatch].pmatch[i].rm_so, patmatch[lowest_patmatch].pmatch[i].rm_eo);
#endif
							i++;
						}
					}
					applylevel(doc, patmatch[lowest_patmatch].pat->childs, istart, iend, patmatch[lowest_patmatch].pmatch,
							   g_strndup(&string[istart - start + byte_char_diff_start], iend - istart));
				}
#ifdef HL_DEBUG
				DEBUG_MSG("applylevel, offset was %d, and will be %d\n", offset,
						  patmatch[lowest_patmatch].pmatch_offset + patmatch[lowest_patmatch].pmatch[0].rm_eo);
#endif
				offset = patmatch[lowest_patmatch].pmatch_offset + patmatch[lowest_patmatch].pmatch[0].rm_eo;

			} else {
				/* so there is no match at all eh? invalidate this pattern ??
				this part is perhaps NOT CORRECT YET
				 */
#ifdef HL_DEBUG
				DEBUG_MSG("applylevel, no match, so settings offset(%d) to %d\n", offset, offset+patmatch[lowest_patmatch].pmatch[0].rm_so +1);
#endif
				offset += patmatch[lowest_patmatch].pmatch[0].rm_so +1;
			}
			
				/* search for the next match for all patterns that did start < offset, 
				we do not need to  check type 3 because it can only run once in every level, so we set it to -1 */
				{
					gint i;
					for (i = 0; i < numpats; i++) {
						if (patmatch[i].pat->mode == 3) {
							patmatch[i].pmatch[0].rm_so = -1;
						} else {
							if (patmatch[i].pmatch[0].rm_so >= 0 && patmatch[i].pmatch[0].rm_so + patmatch[i].pmatch_offset < offset) {
								/* re-search this pattern */
								patmatch_rematch(&patmatch[i], offset, par_pmatch, string);
							}
						}
					}
				}
			
			
		}
		g_free(string);
		g_free(patmatch);
	}
}

void doc_remove_highlighting(Tdocument * doc)
{
	GtkTextIter itstart, itend;
	gtk_text_buffer_get_bounds(doc->buffer, &itstart, &itend);
	gtk_text_buffer_remove_all_tags(doc->buffer, &itstart, &itend);
}

void hl_reset_highlighting_type(Tdocument * doc, gchar * newfilename)
{
	Tfiletype *filetype = hl_get_highlightset_by_filename(newfilename);
	if (filetype && filetype->highlightlist) {
		if (filetype != doc->hl) {
			doc_remove_highlighting(doc);
			doc->hl = filetype;
			doc->need_highlighting = TRUE;
			menu_current_document_type_set_active_wo_activate(filetype);
		}
	}
}

gboolean hl_set_highlighting_type(Tdocument * doc, Tfiletype *filetype) {
	if (filetype && filetype->highlightlist) {
		if (filetype != doc->hl) {
			doc_remove_highlighting(doc);
			doc->hl = filetype;
			doc->need_highlighting = TRUE;
			menu_current_document_type_set_active_wo_activate(filetype);
		}
		return TRUE;
	} else {
		return FALSE;
	}
}

void doc_highlight_full(Tdocument * doc)
{
#ifdef HL_TIMING
	struct tms tms1, tms2;
#endif
	gint so = 0, eo = gtk_text_buffer_get_char_count(doc->buffer);
	doc_remove_highlighting(doc);
#ifdef DEBUG
	g_assert(doc);
	g_assert(doc->hl);
#endif
#ifdef HL_TIMING
	times(&tms1);
#endif
	applylevel(doc, doc->hl->highlightlist, so, eo, NULL, NULL);
#ifdef HL_TIMING
	times(&tms2);
	g_print("doc_highlight_full done, %d ms user-time needed for highlighting\n",
			  (int) (double) ((tms2.tms_utime - tms1.tms_utime) * 1000 / sysconf(_SC_CLK_TCK)));
#endif
	doc->need_highlighting = FALSE;
}

static void remove_tag_by_list_in_region(Tdocument * doc, GList * patlist, GtkTextIter * itstart, GtkTextIter * itend)
{
	GList *tmplist = g_list_first(patlist);
	/* remove all tags that are children of patlist */
	while (tmplist) {
		gtk_text_buffer_remove_tag(doc->buffer, ((Tpattern *) tmplist->data)->tag, itstart, itend);
		if (((Tpattern *) tmplist->data)->childs) {
			remove_tag_by_list_in_region(doc, ((Tpattern *) tmplist->data)->childs, itstart, itend);
		}
		tmplist = g_list_next(tmplist);
	}
}

static Tpattern *find_pattern_by_tag(GList * parentlist, GtkTextTag * tag)
{
	GList *tmplist;

	tmplist = g_list_first(parentlist);
	while (tmplist) {
		if (((Tpattern *) tmplist->data)->tag == tag) {
			return (Tpattern *) tmplist->data;
		}
		tmplist = g_list_next(tmplist);
	}
	return NULL;
}

void doc_highlight_line(Tdocument * doc)
{
#ifdef HL_TIMING
	struct tms tms1, tms2;
#endif
	gint so, eo;
	Tpattern *pat = NULL;
	GList *patternlist = doc->hl->highlightlist;
	{
		GtkTextIter itstart, itend, itsearch;
		GSList *taglist = NULL, *slist;
		GtkTextMark *mark = gtk_text_buffer_get_insert(doc->buffer);
		gtk_text_buffer_get_iter_at_mark(doc->buffer, &itstart, mark);
		/* move to the beginning of the line */
		gtk_text_iter_set_line_offset(&itstart, 0);
		if (main_v->props.highlight_num_lines_count) {
			gtk_text_iter_backward_lines(&itstart, main_v->props.highlight_num_lines_count);
		}
#ifdef DEBUG
		DEBUG_MSG("doc_highlight_line, itstart is at %d\n", gtk_text_iter_get_offset(&itstart));
#endif
		gtk_text_buffer_get_iter_at_mark(doc->buffer, &itend, mark);
/*		gtk_text_iter_forward_to_line_end(&itend);
		gtk_text_iter_set_line_offset(&itend, 0);*/
		if (main_v->props.highlight_num_lines_count) {
			gtk_text_iter_forward_lines(&itend, main_v->props.highlight_num_lines_count);
		}
		if (gtk_text_iter_forward_to_line_end(&itend)) {
			gtk_text_iter_forward_char(&itend);
		}
#ifdef DEBUG
		DEBUG_MSG("doc_highlight_line, itend is at %d\n", gtk_text_iter_get_offset(&itend));
#endif
		if (gtk_text_iter_get_offset(&itstart) == gtk_text_iter_get_offset(&itend)) {
			DEBUG_MSG("doc_highlight_line start end end are the same, cannot highlight!!\n");
			return;
		}

		/* get all the tags that itstart is in */
		taglist = gtk_text_iter_get_tags(&itstart);
		/* find for every tag if it ends _after_ itend or not */
		itsearch = itstart;
		slist = taglist;
		while (slist && slist->data) {
			gboolean tag_found;
#ifdef DEBUG
			{
				Tpattern *testpat;
				testpat = find_pattern_by_tag(patternlist, GTK_TEXT_TAG(slist->data));
				if (testpat) {
					DEBUG_MSG("doc_highlight_line, testpat %p (%s) has tag %p\n", testpat, testpat->name, testpat->tag);
				}
			}
			{
				gchar *test = gtk_text_buffer_get_text(doc->buffer, &itstart, &itend,
													   FALSE);
				DEBUG_MSG("doc_highlight_line, current string=%s\n", test);
				g_free(test);
			}
#endif
			/* if the tags ends at itstart there is no need to search forward to the end */
			if (!gtk_text_iter_ends_tag(&itstart, GTK_TEXT_TAG(slist->data))) {
				DEBUG_MSG("doc_highlight_line, looking for tag %p from so=%d to eo=%d\n", slist->data,
						  gtk_text_iter_get_offset(&itstart), gtk_text_iter_get_offset(&itend));
				tag_found = gtk_text_iter_forward_to_tag_toggle(&itsearch, GTK_TEXT_TAG(slist->data));
				if (!tag_found) {
					DEBUG_MSG("doc_highlight_line, very weird situation, the tag is started but it doesn't end ??\n");
					exit(1);
				} else {
					DEBUG_MSG("doc_highlight_line, tag %p found at itsearch=%d\n", slist->data,
							  gtk_text_iter_get_offset(&itsearch));
					if (gtk_text_iter_compare(&itsearch, &itend) > 0) {
						/* both the start and endpoint are within this 
						   tag --> pattern matching can start with this
						   subpattern */

						pat = find_pattern_by_tag(patternlist, GTK_TEXT_TAG(slist->data));
						DEBUG_MSG("found pattern %p (%s) with tag %p and childs %p\n", pat, pat->name, pat->tag,
								  pat->childs);
						if (pat && (pat->mode == 1 || pat->mode == 0)) {
							gchar *string;
							regmatch_t pmatch[1];
							/* but first we do a quick test if the parent-pattern is indeed still valid */
							string = gtk_text_buffer_get_text(doc->buffer, &itstart, &itend, FALSE);
							if (0 == regexec(&pat->rpat2, string, 1, pmatch, 0)) {
								/* the current line does not have the start of the tag or the end of the tag, but now 
								   it does have a match on the end pattern --> so the pattern should be invalidated */
								pat = NULL;
								DEBUG_MSG
									("doc_highlight_line, a match of the endpattern is found on this line '%s', the pattern is invalidated\n",
									 string);
								itend = itsearch;
								gtk_text_iter_backward_to_tag_toggle(&itstart, slist->data);
							}
							g_free(string);
						}
						if (pat) {
							/* what happens if patternlist = NULL ?
							   that means we are inside a match without any subpatterns, but 
							   perhaps the subpattern should be invalidated... hmm..
							 */
							patternlist = pat->childs;
							DEBUG_MSG("doc_highlight_line, patternlist to use is %p\n", patternlist);
						}
					} else {
						/* this tag stops somewhere in the middle of the line, move 
						   itstart to the beginning of this tag */
						if (gtk_text_iter_begins_tag(&itstart, GTK_TEXT_TAG(slist->data))) {
							DEBUG_MSG("doc_highlight_line, itstart at %d is already at the beginning of tag %p\n",
									  gtk_text_iter_get_offset(&itstart), slist->data);
						} else {
							DEBUG_MSG("doc_highlight_line, move itstart from %d to beginning of tag %p\n",
									  gtk_text_iter_get_offset(&itstart), slist->data);
							gtk_text_iter_backward_to_tag_toggle(&itstart, GTK_TEXT_TAG(slist->data));
							DEBUG_MSG("doc_highlight_line, itstart is set back to %d\n", gtk_text_iter_get_offset(&itstart));
						}
					}
				}
			}
			itsearch = itstart;
			slist = g_slist_next(slist);
		}
		g_slist_free(taglist);

		/* get all the tags that itend is in */
		taglist = gtk_text_iter_get_tags(&itend);
		/* find for every tag if it startss _before_ itstart or not */
		itsearch = itend;
		slist = taglist;
		while (slist && slist->data) {
			gboolean tag_found;
					/* if the tags starts at itend there is no need to search backward to the start */
			if (!gtk_text_iter_begins_tag(&itend, GTK_TEXT_TAG(slist->data))) {
				DEBUG_MSG("doc_highlight_line, looking for tag %p from so=%d to eo=%d\n", slist->data,
						  gtk_text_iter_get_offset(&itstart), gtk_text_iter_get_offset(&itend));
				tag_found = gtk_text_iter_backward_to_tag_toggle(&itsearch, GTK_TEXT_TAG(slist->data));
				if (!tag_found) {
					DEBUG_MSG("doc_highlight_line, very weird situation, the tag is started but it doesn't end ??\n");
					exit(1);
				} else {
					DEBUG_MSG("doc_highlight_line, tag %p found at itsearch=%d\n", slist->data,
							  gtk_text_iter_get_offset(&itsearch));
					if (gtk_text_iter_compare(&itsearch, &itstart) <= 0) {
						/* both the start and endpoint are within this 
						   tag --> pattern matching can start with this
						   subpattern,	since we did run the same algorithm for itstart we can skip a bit now */
					} else {
						/* this tag starts somewhere in the middle of the line, move 
						   itend to the end of this tag */
						if (gtk_text_iter_ends_tag(&itend, GTK_TEXT_TAG(slist->data))) {
							DEBUG_MSG("doc_highlight_line, itend at %d is already at the end of tag %p\n",
									  gtk_text_iter_get_offset(&itend), slist->data);
						} else {
							DEBUG_MSG("doc_highlight_line, move itend from %d to end of tag %p\n",
									  gtk_text_iter_get_offset(&itend), slist->data);
							gtk_text_iter_forward_to_tag_toggle(&itend, GTK_TEXT_TAG(slist->data));
							DEBUG_MSG("doc_highlight_line, itend is set forward to %d\n", gtk_text_iter_get_offset(&itend));
						}
					}
				}
			}
			itsearch = itend;
			slist = g_slist_next(slist);
		}
		
		
		/* this function removes some specific tags from the region */
		remove_tag_by_list_in_region(doc, patternlist, &itstart, &itend);
		so = gtk_text_iter_get_offset(&itstart);
		eo = gtk_text_iter_get_offset(&itend);
	}

	if (patternlist) {
		DEBUG_MSG("doc_highlight_line from so=%d to eo=%d\n", so, eo);
#ifdef HL_TIMING
		times(&tms1);
#endif
		applylevel(doc, patternlist, so, eo, NULL, NULL);
#ifdef HL_TIMING
		times(&tms2);
		g_print("doc_highlight_line done, %d ms user-time needed for highlighting\n",
				(int) (double) ((tms2.tms_utime - tms1.tms_utime) * 1000 / sysconf(_SC_CLK_TCK)));
#endif
	} else {
		DEBUG_MSG("doc_highlight_line, patternlist=NULL so we don't need highlighting\n");
	}
	doc->need_highlighting = FALSE;
}

#define MAKE_BLUEFISH_WITH_BLUEFISH

void hl_reset_to_default()
{
	gchar **arr;

#ifdef MAKE_BLUEFISH_WITH_BLUEFISH
	arr = array_from_arglist("c", "string", "0", "\"", "\"", "", "1", "", "#009900", "", "0", "0", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr =
		array_from_arglist("c", "preprocessor", "0", "#(include|define|if|ifdef|else|endif)[^\n]*\n", "", "", "2", "",
						   "#000099", "", "0", "0", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("c", "comment", "0", "/\\*", "\\*/", "", "1", "^(top|preprocessor)$", "#AAAAAA", "", "1", "2", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr =
		array_from_arglist("c", "keywords", "0",
						   "(return|goto|if|else|case|default|switch|break|continue|while|do|for|sizeof)",
						   "", "", "2", "", "#000000", "", "2", "0", " \t\n*{};", " \t\n*{};", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr =
		array_from_arglist("c", "storage-keywords", "0",
						   "(const|extern|auto|register|static|unsigned|signed|volatile|char|double|float|int|long|short|void|typedef|struct|union|enum|FILE|gint|gchar|GList|GtkWidget|gpointer|guint|gboolean)",
						   "", "", "2", "", "#990000", "", "2", "0", " \t\n*;", " \t\n*;", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("c", "braces", "0", "[{()}]", "", "", "2", "", "#000000", "", "2", "0", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("c", "character", "0", "'", "'", "", "1", "", "#009900", "", "0", "0", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
#endif

	/* the default php pattern */
	arr = array_from_arglist("php", "html", "1", "<((/)?[a-z0-9]+)", "[^?-]>", "", "1", "", "#000077", "", "0", "0", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("php", "html-tag", "1", "1", "", "", "3", "^html$", "#550044", "", "2", "0", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr =
		array_from_arglist("php", "html-tag-table", "1", "^(/)?(table|td|tr|tbody)$", "", "", "2", "^html-tag$", "#5005AA",
						   "", "0", "0", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr =
		array_from_arglist("php", "html-tag-special", "1", "^(/)?(img|a)$", "", "", "2", "^html-tag$", "#BB0540", "", "0",
						   "0", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr =
		array_from_arglist("php", "html-attrib", "1", "([a-z]*=)(\"[^\"]*\")", "", "", "2", "^html$", "", "", "0", "0",
						   "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr =
		array_from_arglist("php", "html-attrib-sub2", "1", "2", "", "", "3", "^html-attrib$", "#009900", "", "0", "0", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("php", "specialchar", "1", "&[^;]*;", "", "", "2", "", "#999999", "", "2", "0", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("php", "comment", "0", "<!--", "-->", "", "1", "", "#AAAAAA", "", "1", "2", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr =
		array_from_arglist("php", "php", "1", "<\\?php", "\\?>", "", "1", "^(top|html|html-attrib-sub2)$", "#0000FF", "",
						   "0", "0", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr =
		array_from_arglist("php", "php-keywords", "0",
						   "(return|goto|if|else|case|default|switch|break|continue|while|do|for|global|var|class|function|new)",
						   "", "", "2", "^php$", "#000000", "", "2", "0", " \t\n", " \t\n;:", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("php", "php-braces", "0", "[{()}]", "", "", "2", "^php$", "#000000", "", "2", "0", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("php", "php-string-double", "0", "\"", "\"", "", "1", "^php$", "#009900", "", "1", "1", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr =
		array_from_arglist("php", "php-var", "1", "\\$[a-z][][a-z0-9>_$-]*", "", "", "2", "^php$", "#CC0000", "", "2", "0",
						   "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr =
		array_from_arglist("php", "php-var-specialchars", "0", "(\\[|\\]|->)", "", "", "2", "^php-var$", "#0000CC", "", "0",
						   "0", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("php", "php-string-single", "0", "'", "'", "", "1", "^php$", "#009900", "", "1", "1", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("php", "php-comment-C", "0", "/\\*", "\\*/", "", "1", "^php$", "#7777AA", "", "1", "2", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr =
		array_from_arglist("php", "php-comment-C++", "0", "//[^\n]*\n", "", "", "2", "^php$", "#7777AA", "", "1", "2", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	DEBUG_MSG("hl_reset_to_default, done\n");

	/* the default HTML pattern */
	arr = array_from_arglist("html", "html", "1", "<((/)?[a-z0-9]+)", "[^?-]>", "", "1", "", "#0000EE", "", "0", "0", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("html", "html-tag", "1", "1", "", "", "3", "^html$", "#000077", "", "2", "0", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("html", "html-attrib", "1", "([a-z]*=)(\"[^\"]*\")", "", "", "2", "^html$", "", "", "0", "0","", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("html", "html-attrib-sub2", "1", "2", "", "", "3", "^html-attrib$", "#009900", "", "0", "0", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("html", "specialchar", "1", "&[^;]*;", "", "", "2", "", "#999999", "", "2", "0", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("html", "comment", "0", "<!--", "-->", "", "1", "", "#AAAAAA", "", "1", "2", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("html", "doctype", "1", "<![a-z0-9]+", "[^?-]>", "", "1", "", "#bb8800", "", "0", "0", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);

	/* the default XML pattern */
	arr = array_from_arglist("xml", "tag", "1", "<((/)?[a-z0-9]+)", "[^?-]>", "", "1", "", "#0000EE", "", "0", "0", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("xml", "tag-name", "1", "1", "", "", "3", "^html$", "#000077", "", "2", "0", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("xml", "tag-attrib", "1", "([a-z]*=)(\"[^\"]*\")", "", "", "2", "^tag$", "", "", "0", "0","", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("xml", "tag-attrib-sub2", "1", "2", "", "", "3", "^tag-attrib$", "#009900", "", "0", "0", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("xml", "specialchar", "1", "&[^;]*;", "", "", "2", "", "#999999", "", "2", "0", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("xml", "comment", "0", "<!--", "-->", "", "1", "", "#AAAAAA", "", "1", "2", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("xml", "doctype", "1", "<![a-z0-9]+", "[^?-]>", "", "1", "", "#bb8800", "", "0", "0", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);

	/* the default java pattern */
	arr = array_from_arglist("java", "comment-C", "0", "/\\*", "\\*/", "", "1", "", "#AAAAAA", "", "1", "2", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("java", "comment-C++", "0", "//[^\n]*\n", "", "", "2", "", "#AAAAAA", "", "1", "2", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("java", "string", "0", "\"", "\"", "", "1", "", "#009900", "", "0", "0", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr =
		array_from_arglist("java", "include", "0", "(import|package)[^\n]*\n", "", "", "2", "", "#000099", "", "0", "0",
						   "\n", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr =
		array_from_arglist("java", "keywords", "0",
						   "(new|super|return|goto|if|else|case|default|switch|break|continue|while|do|for|catch|throw|finally|try|class)",
						   "", "", "2", "", "#000000", "", "2", "0", " \t\n*};", " \t\n{:;", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr =
		array_from_arglist("java", "special_values", "0", "(false|null|true)", "", "", "2", "", "#5caeee",
						   "", "0", "0", " \t\n=(", " \t\n);,", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr =
		array_from_arglist("java", "modifiers", "0",
						   "(abstract|final|native|private|protected|public|static|transient|synchronized|volatile|extends|implements)",
						   "", "", "2", "", "#990000", "", "2", "0", " \t\n", " \t\n", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("java", "this", "0", "this\\.", "", "", "2", "", "#000099", "", "2", "0", "=+*/ \n\t(", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr =
		array_from_arglist("java", "primitive-types", "0", "(void|double|boolean|int)", "", "", "2", "", "#880088", "", "2",
						   "0", " \t\n(,", " \t\n,;)", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("java", "braces", "0", "[{()}]", "", "", "2", "", "#000000", "", "2", "0", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("java", "character", "0", "'", "'", "", "1", "", "#009900", "", "0", "0", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);

	arr = array_from_arglist("sql", "braces", "0", "[{()}]", "", "", "2", "", "#000000", "", "2", "0", "", "", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("sql", "commands", "1","(create|alter|insert|update|delete)","", "", "2", "", "#990000", "", "2", "0", " \t\n", " \t\n", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("sql", "keywords", "1","(references|cascade|not null|null|default|table|user|foreign key|index|unique|primary key|constraint|alter|values)","", "", "2", "", "#990000", "", "2", "0", " \t\n", " \t\n", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
	arr = array_from_arglist("sql", "storage", "1", "(integer|varchar|char|blob|double|timestamp)", "", "", "2", "", "#880088", "", "2","0", " \t\n", " \t\n", NULL);
	main_v->props.highlight_patterns = g_list_append(main_v->props.highlight_patterns, arr);
}

GtkTextTagTable *highlight_return_tagtable() {
	return highlight.tagtable;
}
