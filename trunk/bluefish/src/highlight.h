void hl_init();
Tfiletype *hl_get_highlightset_by_type(gchar *type);
Tfiletype *hl_get_highlightset_by_filename(gchar *filename);
void hl_reset_highlighting_type(Tdocument * doc, gchar * newfilename);
gboolean hl_set_highlighting_type(Tdocument * doc, Tfiletype *filetype);
void doc_highlight_full(Tdocument *doc);
void doc_highlight_line(Tdocument *doc);
void doc_remove_highlighting(Tdocument *doc);
void hl_reset_to_default();

GtkTextTagTable *highlight_return_tagtable();


