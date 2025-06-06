/*
 *  prefs_editor_gtk.cpp - Preferences editor, Unix implementation using GTK+
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sysdeps.h"

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <gtk/gtk.h>

#include <shellapi.h>

#include <fstream>

#include "user_strings.h"
#include "version.h"
#include "cdrom.h"
#include "xpram.h"
#include "prefs.h"
#include "prefs_editor.h"
#include "util_windows.h"
#include "b2ether/inc/b2ether_hl.h"


// Global variables
static GtkWidget *win;				// Preferences window
static bool start_clicked = true;	// Return value of PrefsEditor() function


// Prototypes
static void create_volumes_pane(GtkWidget *top);
static void create_scsi_pane(GtkWidget *top);
static void create_graphics_pane(GtkWidget *top);
static void create_input_pane(GtkWidget *top);
static void create_serial_pane(GtkWidget *top);
static void create_ethernet_pane(GtkWidget *top);
static void create_memory_pane(GtkWidget *top);
static void create_jit_pane(GtkWidget *top);
static void read_settings(void);
static void add_volume_entry_with_type(const char * filename, bool cdrom);
static void add_volume_entry_guessed(const char * filename);
static bool volume_in_list(const char * filename);


/*
 *  SheepShaver glue
 */

#ifdef SHEEPSHAVER
#define DISABLE_SCSI 1
#define PROGRAM_NAME "SheepShaver"
enum {
	STR_WINDOW_LAB = STR_WINDOW_CTRL,
	STR_FULLSCREEN_LAB = STR_FULLSCREEN_CTRL,
	STR_SERIALA_CTRL = STR_SERPORTA_CTRL,
	STR_SERIALB_CTRL = STR_SERPORTB_CTRL,
};
#else
#define DISABLE_SCSI 1 /* XXX merge code from original Basilisk II for Windows */
#define PROGRAM_NAME "BasiliskII"
#endif


/*
 *  Utility functions
 */

gchar * tchar_to_g_utf8(const TCHAR * str) {
	gchar * out;
	if (str == NULL)
		return NULL;
	int len = _tcslen(str) + 1;
	#ifdef _UNICODE
		/* First call just to find what the output size will be */
		int size = WideCharToMultiByte(CP_UTF8, 0, str, len, NULL, 0, NULL, NULL);
		if (size == 0)
			return NULL;
		out = (gchar *) g_malloc(size);
		if (out == NULL)
			return NULL;
		WideCharToMultiByte(CP_UTF8, 0, str, len, out, size, NULL, NULL);
	#else /* _UNICODE */
		out = g_locale_to_utf8(str, -1, NULL, NULL, NULL);
	#endif /* _UNICODE */
	return out;
}


struct opt_desc {
	opt_desc(int l, GCallback f, GtkWidget **s=NULL) : label_id(l), func(f), save_ref(s) {}

	int label_id;
	GtkSignalFunc func;
	GtkWidget ** save_ref;
};

struct combo_desc {
	int label_id;
};

struct file_req_assoc {
	file_req_assoc(GtkWidget *r, GtkWidget *e) : req(r), entry(e) {}
	GtkWidget *req;
	GtkWidget *entry;
};

static void cb_browse_ok(GtkWidget *button, file_req_assoc *assoc)
{
	gchar *file = (char *)gtk_file_selection_get_filename(GTK_FILE_SELECTION(assoc->req));
	gtk_entry_set_text(GTK_ENTRY(assoc->entry), file);
	gtk_widget_destroy(assoc->req);
	delete assoc;
}

static void cb_browse(GtkWidget *widget, void *user_data)
{
	GtkWidget *req = gtk_file_selection_new(GetString(STR_BROWSE_TITLE));
	gtk_signal_connect_object(GTK_OBJECT(req), "delete_event", GTK_SIGNAL_FUNC(gtk_widget_destroy), GTK_OBJECT(req));
	gtk_signal_connect(GTK_OBJECT(GTK_FILE_SELECTION(req)->ok_button), "clicked", GTK_SIGNAL_FUNC(cb_browse_ok), new file_req_assoc(req, (GtkWidget *)user_data));
	gtk_signal_connect_object(GTK_OBJECT(GTK_FILE_SELECTION(req)->cancel_button), "clicked", GTK_SIGNAL_FUNC(gtk_widget_destroy), GTK_OBJECT(req));
	gtk_widget_show(req);
}

static GtkWidget *make_browse_button(GtkWidget *entry)
{
	GtkWidget *button;

	button = gtk_button_new_with_label(GetString(STR_BROWSE_CTRL));
	gtk_widget_show(button);
	gtk_signal_connect(GTK_OBJECT(button), "clicked", (GtkSignalFunc)cb_browse, (void *)entry);
	return button;
}

static void add_menu_item(GtkWidget *menu, const char *label, GtkSignalFunc func, gpointer data = NULL)
{
	GtkWidget *item = gtk_menu_item_new_with_label(label);
	gtk_widget_show(item);
	gtk_signal_connect(GTK_OBJECT(item), "activate", func, data);
	gtk_menu_append(GTK_MENU(menu), item);
}

static void add_menu_item(GtkWidget *menu, int label_id, GtkSignalFunc func)
{
	add_menu_item(menu, GetString(label_id), func, NULL);
}

static GtkWidget *make_pane(GtkWidget *notebook, int title_id)
{
	GtkWidget *frame, *label, *box;

	frame = gtk_frame_new(NULL);
	gtk_widget_show(frame);
	gtk_container_border_width(GTK_CONTAINER(frame), 4);

	label = gtk_label_new(GetString(title_id));
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook), frame, label);

	box = gtk_vbox_new(FALSE, 4);
	gtk_widget_show(box);
	gtk_container_set_border_width(GTK_CONTAINER(box), 4);
	gtk_container_add(GTK_CONTAINER(frame), box);
	return box;
}

static GtkWidget *make_button_box(GtkWidget *top, int border, const opt_desc *buttons)
{
	GtkWidget *bb, *button;

	bb = gtk_hbutton_box_new();
	gtk_widget_show(bb);
	gtk_container_set_border_width(GTK_CONTAINER(bb), border);
	gtk_button_box_set_layout(GTK_BUTTON_BOX(bb), GTK_BUTTONBOX_DEFAULT_STYLE);
	gtk_button_box_set_spacing(GTK_BUTTON_BOX(bb), 4);
	gtk_box_pack_start(GTK_BOX(top), bb, FALSE, FALSE, 0);

	while (buttons->label_id) {
		button = gtk_button_new_with_label(GetString(buttons->label_id));
		gtk_widget_show(button);
		gtk_signal_connect_object(GTK_OBJECT(button), "clicked", buttons->func, NULL);
		gtk_box_pack_start(GTK_BOX(bb), button, TRUE, TRUE, 0);
		if (buttons->save_ref) {
			*(buttons->save_ref) = button;
		}
		buttons++;
	}
	return bb;
}

static GtkWidget *make_separator(GtkWidget *top)
{
	GtkWidget *sep = gtk_hseparator_new();
	gtk_box_pack_start(GTK_BOX(top), sep, FALSE, FALSE, 0);
	gtk_widget_show(sep);
	return sep;
}

static GtkWidget *make_table(GtkWidget *top, int x, int y)
{
	GtkWidget *table = gtk_table_new(x, y, FALSE);
	gtk_widget_show(table);
	gtk_box_pack_start(GTK_BOX(top), table, FALSE, FALSE, 0);
	return table;
}

static GtkWidget *table_make_option_menu(GtkWidget *table, int row, int label_id, const opt_desc *options, int active)
{
	GtkWidget *label, *opt, *menu;

	label = gtk_label_new(GetString(label_id));
	gtk_widget_show(label);
	gtk_table_attach(GTK_TABLE(table), label, 0, 1, row, row + 1, (GtkAttachOptions)0, (GtkAttachOptions)0, 4, 4);

	opt = gtk_option_menu_new();
	gtk_widget_show(opt);
	menu = gtk_menu_new();

	while (options->label_id) {
		add_menu_item(menu, options->label_id, options->func);
		options++;
	}
	gtk_menu_set_active(GTK_MENU(menu), active);

	gtk_option_menu_set_menu(GTK_OPTION_MENU(opt), menu);
	gtk_table_attach(GTK_TABLE(table), opt, 1, 2, row, row + 1, (GtkAttachOptions)(GTK_FILL | GTK_EXPAND), (GtkAttachOptions)0, 4, 4);
	return menu;
}

static GtkWidget *table_make_combobox(GtkWidget *table, int row, int label_id, const char *default_value, GList *glist)
{
	GtkWidget *label, *combo;
	char str[32];

	label = gtk_label_new(GetString(label_id));
	gtk_widget_show(label);
	gtk_table_attach(GTK_TABLE(table), label, 0, 1, row, row + 1, (GtkAttachOptions)0, (GtkAttachOptions)0, 4, 4);
	
	combo = gtk_combo_new();
	gtk_widget_show(combo);
	gtk_combo_set_popdown_strings(GTK_COMBO(combo), glist);

	gtk_entry_set_text(GTK_ENTRY(GTK_COMBO(combo)->entry), default_value);
	gtk_table_attach(GTK_TABLE(table), combo, 1, 2, row, row + 1, (GtkAttachOptions)(GTK_FILL | GTK_EXPAND), (GtkAttachOptions)0, 4, 4);
	
	return combo;
}

static GtkWidget *table_make_combobox(GtkWidget *table, int row, int label_id, const char *default_value, const combo_desc *options)
{
	GList *glist = NULL;
	while (options->label_id) {
		glist = g_list_append(glist, (void *)GetString(options->label_id));
		options++;
	}

	return table_make_combobox(table, row, label_id, default_value, glist);
}

static GtkWidget *table_make_file_entry(GtkWidget *table, int row, int label_id, const char *prefs_item, bool only_dirs = false)
{
	GtkWidget *box, *label, *entry, *button;

	label = gtk_label_new(GetString(label_id));
	gtk_widget_show(label);
	gtk_table_attach(GTK_TABLE(table), label, 0, 1, row, row + 1, (GtkAttachOptions)0, (GtkAttachOptions)0, 4, 4);

	const char *str = PrefsFindString(prefs_item);
	if (str == NULL)
		str = "";

	box = gtk_hbox_new(FALSE, 4);
	gtk_widget_show(box);
	gtk_table_attach(GTK_TABLE(table), box, 1, 2, row, row + 1, (GtkAttachOptions)(GTK_FILL | GTK_EXPAND), (GtkAttachOptions)0, 4, 4);

	entry = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(entry), str); 
	gtk_widget_show(entry);
	gtk_box_pack_start(GTK_BOX(box), entry, TRUE, TRUE, 0);

	button = make_browse_button(entry);
	gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
	g_object_set_data(G_OBJECT(entry), "chooser_button", button);
	return entry;
}

static GtkWidget *make_option_menu(GtkWidget *top, int label_id, const opt_desc *options, int active)
{
	GtkWidget *box, *label, *opt, *menu;

	box = gtk_hbox_new(FALSE, 4);
	gtk_widget_show(box);
	gtk_box_pack_start(GTK_BOX(top), box, FALSE, FALSE, 0);

	label = gtk_label_new(GetString(label_id));
	gtk_widget_show(label);
	gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

	opt = gtk_option_menu_new();
	gtk_widget_show(opt);
	menu = gtk_menu_new();

	while (options->label_id) {
		add_menu_item(menu, options->label_id, options->func);
		options++;
	}
	gtk_menu_set_active(GTK_MENU(menu), active);

	gtk_option_menu_set_menu(GTK_OPTION_MENU(opt), menu);
	gtk_box_pack_start(GTK_BOX(box), opt, FALSE, FALSE, 0);
	return menu;
}

static GtkWidget *make_file_entry(GtkWidget *top, int label_id, const char *prefs_item, bool only_dirs = false)
{
	GtkWidget *box, *label, *entry;

	box = gtk_hbox_new(FALSE, 4);
	gtk_widget_show(box);
	gtk_box_pack_start(GTK_BOX(top), box, FALSE, FALSE, 0);

	label = gtk_label_new(GetString(label_id));
	gtk_widget_show(label);
	gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

	const char *str = PrefsFindString(prefs_item);
	if (str == NULL)
		str = "";

	entry = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(entry), str); 
	gtk_widget_show(entry);
	gtk_box_pack_start(GTK_BOX(box), entry, TRUE, TRUE, 0);
	return entry;
}

static const gchar *get_file_entry_path(GtkWidget *entry)
{
	return gtk_entry_get_text(GTK_ENTRY(entry));
}

static GtkWidget *make_checkbox(GtkWidget *top, int label_id, const char *prefs_item, GtkSignalFunc func)
{
	GtkWidget *button = gtk_check_button_new_with_label(GetString(label_id));
	gtk_widget_show(button);
	gtk_toggle_button_set_state(GTK_TOGGLE_BUTTON(button), PrefsFindBool(prefs_item));
	gtk_signal_connect(GTK_OBJECT(button), "toggled", func, button);
	gtk_box_pack_start(GTK_BOX(top), button, FALSE, FALSE, 0);
	return button;
}

static GtkWidget *make_combobox(GtkWidget *top, int label_id, const char *default_value, GList *glist)
{
	GtkWidget *box, *label, *combo;

	box = gtk_hbox_new(FALSE, 4);
	gtk_widget_show(box);
	gtk_box_pack_start(GTK_BOX(top), box, FALSE, FALSE, 0);

	label = gtk_label_new(GetString(label_id));
	gtk_widget_show(label);
	gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
	
	combo = gtk_combo_new();
	gtk_widget_show(combo);
	gtk_combo_set_popdown_strings(GTK_COMBO(combo), glist);
	
	gtk_entry_set_text(GTK_ENTRY(GTK_COMBO(combo)->entry), default_value);
	gtk_box_pack_start(GTK_BOX(box), combo, TRUE, TRUE, 0);
	
	return combo;
}

static GtkWidget *make_combobox(GtkWidget *top, int label_id, const char *default_value, const combo_desc *options)
{
	GList *glist = NULL;
	while (options->label_id) {
		glist = g_list_append(glist, (void *)GetString(options->label_id));
		options++;
	}

	return make_combobox(top, label_id, default_value, glist);
}

 
/*
 *  Show preferences editor
 *  Returns true when user clicked on "Start", false otherwise
 */

// Window closed
static gint window_closed(void)
{
	start_clicked = false;
	return FALSE;
}

// Window destroyed
static void window_destroyed(void)
{
	gtk_main_quit();
}

// "Start" button clicked
static void cb_start(...)
{
	start_clicked = true;
	read_settings();
	SavePrefs();
	gtk_widget_destroy(win);
}

// "Zap PRAM" button clicked
static void cb_zap_pram(...)
{
	ZapPRAM();
}

// "Quit" button clicked
static void cb_quit(...)
{
	start_clicked = false;
	gtk_widget_destroy(win);
}

// "OK" button of "About" dialog clicked
static void dl_quit(GtkWidget *dialog)
{
	gtk_widget_destroy(dialog);
}

// "About" button clicked
static void cb_about(...)
{
	GtkWidget *dialog;

	GtkWidget *label, *button;

	char str[512];
	sprintf(str,
		PROGRAM_NAME "\nVersion %d.%d\n\n"
		"Copyright (C) 1997-2008 Christian Bauer et al.\n"
		"E-mail: cb@cebix.net\n"
#ifdef SHEEPSHAVER
		"http://sheepshaver.cebix.net/\n\n"
#else
		"http://basilisk.cebix.net/\n\n"
#endif
		PROGRAM_NAME " comes with ABSOLUTELY NO\n"
		"WARRANTY. This is free software, and\n"
		"you are welcome to redistribute it\n"
		"under the terms of the GNU General\n"
		"Public License.\n",
		VERSION_MAJOR, VERSION_MINOR
	);

	dialog = gtk_dialog_new();
	gtk_window_set_title(GTK_WINDOW(dialog), GetString(STR_ABOUT_TITLE));
	gtk_container_border_width(GTK_CONTAINER(dialog), 5);
	gtk_widget_set_uposition(GTK_WIDGET(dialog), 100, 150);

	label = gtk_label_new(str);
	gtk_widget_show(label);
	gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->vbox), label, TRUE, TRUE, 0);

	button = gtk_button_new_with_label(GetString(STR_OK_BUTTON));
	gtk_widget_show(button);
	gtk_signal_connect_object(GTK_OBJECT(button), "clicked", GTK_SIGNAL_FUNC(dl_quit), GTK_OBJECT(dialog));
	gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->action_area), button, FALSE, FALSE, 0);
	GTK_WIDGET_SET_FLAGS(button, GTK_CAN_DEFAULT);
	gtk_widget_grab_default(button);

	gtk_widget_show(dialog);
}

// Menu item descriptions
static GtkItemFactoryEntry menu_items[] = {
	{(gchar *)GetString(STR_PREFS_MENU_FILE_GTK),		NULL,			NULL,							0, "<Branch>"},
	{(gchar *)GetString(STR_PREFS_ITEM_START_GTK),		"<control>S",	GTK_SIGNAL_FUNC(cb_start),		0, NULL},
	{(gchar *)GetString(STR_PREFS_ITEM_ZAP_PRAM_GTK),	NULL,			GTK_SIGNAL_FUNC(cb_zap_pram),	0, NULL},
	{(gchar *)GetString(STR_PREFS_ITEM_SEPL_GTK),		NULL,			NULL,							0, "<Separator>"},
	{(gchar *)GetString(STR_PREFS_ITEM_QUIT_GTK),		"<control>Q",	GTK_SIGNAL_FUNC(cb_quit),		0, NULL},
	{(gchar *)GetString(STR_HELP_MENU_GTK),				NULL,			NULL,							0, "<LastBranch>"},
	{(gchar *)GetString(STR_HELP_ITEM_ABOUT_GTK),		"<control>H",	GTK_SIGNAL_FUNC(cb_about),		0, NULL}
};

void PrefsMigrate(void)
{
	// Ethernet
	const char *ether = PrefsFindString("ether");
	if (ether && ether[0] == '{') {
		PrefsReplaceString("etherguid", ether);
		PrefsReplaceString("ether", "b2ether");
	}
	if (PrefsFindBool("routerenabled")) {
		PrefsRemoveItem("etherguid");
		PrefsReplaceString("ether", "router");
	}
}

bool PrefsEditor(void)
{
	// Create window
	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_default_size(GTK_WINDOW(win), 640, 480); // a little bigger than the default
	gtk_window_set_title(GTK_WINDOW(win), GetString(STR_PREFS_TITLE));
	gtk_signal_connect(GTK_OBJECT(win), "delete_event", GTK_SIGNAL_FUNC(window_closed), NULL);
	gtk_signal_connect(GTK_OBJECT(win), "destroy", GTK_SIGNAL_FUNC(window_destroyed), NULL);

	// Create window contents
	GtkWidget *box = gtk_vbox_new(FALSE, 4);
	gtk_widget_show(box);
	gtk_container_add(GTK_CONTAINER(win), box);

	GtkAccelGroup *accel_group = gtk_accel_group_new();
	GtkItemFactory *item_factory = gtk_item_factory_new(GTK_TYPE_MENU_BAR, "<main>", accel_group);
	gtk_item_factory_create_items(item_factory, sizeof(menu_items) / sizeof(menu_items[0]), menu_items, NULL);
#if GTK_CHECK_VERSION(1,3,15)
	gtk_window_add_accel_group(GTK_WINDOW(win), accel_group);
#else
	gtk_accel_group_attach(accel_group, GTK_OBJECT(win));
#endif
	GtkWidget *menu_bar = gtk_item_factory_get_widget(item_factory, "<main>");
	gtk_widget_show(menu_bar);
	gtk_box_pack_start(GTK_BOX(box), menu_bar, FALSE, TRUE, 0);

	GtkWidget *notebook = gtk_notebook_new();
	gtk_widget_show(notebook);
	gtk_notebook_set_tab_pos(GTK_NOTEBOOK(notebook), GTK_POS_TOP);
	gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), FALSE);
	gtk_box_pack_start(GTK_BOX(box), notebook, TRUE, TRUE, 0);

	create_volumes_pane(notebook);
#ifndef DISABLE_SCSI
	create_scsi_pane(notebook);
#endif
	create_graphics_pane(notebook);
	create_input_pane(notebook);
	create_serial_pane(notebook);
	create_ethernet_pane(notebook);
	create_memory_pane(notebook);
	create_jit_pane(notebook);

	static const opt_desc buttons[] = {
		{STR_START_BUTTON, GTK_SIGNAL_FUNC(cb_start)},
		{STR_QUIT_BUTTON, GTK_SIGNAL_FUNC(cb_quit)},
		{0, NULL}
	};
	make_button_box(box, 4, buttons);

	// Show window and enter main loop
	gtk_widget_show(win);
	gtk_main();
	return start_clicked;
}


/*
 *  "Volumes" pane
 */

static GtkWidget *w_enableextfs, *w_extdrives, *w_cdrom_drive;
static GtkWidget *volume_list;
static GtkListStore *volume_list_model;
static GtkWidget *volume_remove_button;

// Set sensitivity of widgets
static void set_volumes_sensitive(void)
{
	const bool enable_extfs = PrefsFindBool("enableextfs");
	gtk_widget_set_sensitive(w_extdrives, enable_extfs);
	const bool no_cdrom = PrefsFindBool("nocdrom");
	gtk_widget_set_sensitive(w_cdrom_drive, !no_cdrom);
}

// Volume list selection changed
static void cl_selected(GtkTreeSelection * selection, gpointer user_data) {
	if (selection) {
		bool have_selection = gtk_tree_selection_get_selected(selection, NULL, NULL);

		gtk_widget_set_sensitive(GTK_WIDGET(volume_remove_button), have_selection);
	}
}

// Process proposed drop
gboolean volume_list_drag_motion (GtkWidget *widget, GdkDragContext *drag_context, gint x, gint y, guint time,
                                            gpointer user_data)
{
	GtkTreePath *path;
	GtkTreeViewDropPosition pos;
	// Don't allow tree-style drops onto, only list-style drops between
	if (gtk_tree_view_get_dest_row_at_pos(GTK_TREE_VIEW(volume_list), x,y, &path, &pos)) {
		switch (pos) {
			case GTK_TREE_VIEW_DROP_INTO_OR_AFTER:
				gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(volume_list), path, GTK_TREE_VIEW_DROP_AFTER);
				break;
			case GTK_TREE_VIEW_DROP_INTO_OR_BEFORE:
				gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(volume_list), path, GTK_TREE_VIEW_DROP_BEFORE);
				break;
			case GTK_TREE_VIEW_DROP_BEFORE:
			case GTK_TREE_VIEW_DROP_AFTER:
				// these are ok, no change
				break;
		}
		gdk_drag_status(drag_context, drag_context->suggested_action, time);
		return TRUE;
	}
	return FALSE;
}

// Something dropped on volume list
static void drag_data_received(GtkWidget *list, GdkDragContext *drag_context, gint x, gint y, GtkSelectionData *data, 
	guint info, guint time, gpointer user_data)
{
	// reordering drags have already been handled by clist
	if (data->type == gdk_atom_intern("gtk-clist-drag-reorder", true)) {
		return;
	}

	// get URIs from the drag selection data and add them
	gchar ** uris = g_strsplit((gchar *)(data->data), "\r\n", -1);
	for (gchar ** uri = uris; *uri != NULL; uri++) {
		if (strlen(*uri) < 7) continue;
		if (strncmp("file://", *uri, 7) != 0) continue;

		gchar * filename = g_filename_from_uri(*uri, NULL, NULL);
		if (filename) {
			add_volume_entry_guessed(filename);

			// figure out where in the list they dropped
			GtkTreePath *path;
			GtkTreeViewDropPosition pos;
			if (gtk_tree_view_get_dest_row_at_pos(GTK_TREE_VIEW(volume_list), x,y, &path, &pos)) {
				GtkTreeIter dest_iter;
				if (gtk_tree_model_get_iter(GTK_TREE_MODEL(volume_list_model), &dest_iter, path)) {

					// Find the item we just added and put it in place
					GtkTreeIter last;
					GtkTreeIter cur;
					if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(volume_list_model), &cur)) {
						do {
							last = cur;
						} while (gtk_tree_model_iter_next(GTK_TREE_MODEL(volume_list_model), &cur));
					}
					switch (pos) {
						case GTK_TREE_VIEW_DROP_AFTER:
						case GTK_TREE_VIEW_DROP_INTO_OR_AFTER:
							gtk_list_store_move_after(volume_list_model, &last, &dest_iter);
							break;
						case GTK_TREE_VIEW_DROP_BEFORE:
						case GTK_TREE_VIEW_DROP_INTO_OR_BEFORE:
							gtk_list_store_move_before(volume_list_model, &last, &dest_iter);
							break;
					}
				}
			}

			g_free(filename);
		}
	}
	g_strfreev(uris);
}

// Volume selected for addition
static void add_volume_ok(GtkWidget *button, file_req_assoc *assoc)
{
	gchar *file = (gchar *)gtk_file_selection_get_filename(GTK_FILE_SELECTION(assoc->req));
	add_volume_entry_guessed(file);
	gtk_widget_destroy(assoc->req);
	delete assoc;
}

// Volume selected for creation
static void create_volume_ok(GtkWidget *button, file_req_assoc *assoc)
{
	gchar *file = (gchar *)gtk_file_selection_get_filename(GTK_FILE_SELECTION(assoc->req));

	const gchar *str = gtk_entry_get_text(GTK_ENTRY(assoc->entry));
	size_t size = atoi(str) << 20;

	int fd = _open(file, _O_WRONLY | _O_CREAT | _O_BINARY | _O_TRUNC, _S_IREAD | _S_IWRITE);
	if (fd >= 0) {
	  bool created_ok = false;
	  if (_chsize(fd, size) == 0)
		created_ok = true;
	  _close(fd);
	  if (created_ok) {
		// A created empty volume is always a new disk
		add_volume_entry_with_type(file, false);
	  }
	}
	gtk_widget_destroy(GTK_WIDGET(assoc->req));
	delete assoc;
}

// "Add Volume" button clicked
static void cb_add_volume(...)
{
	GtkWidget *req = gtk_file_selection_new(GetString(STR_ADD_VOLUME_TITLE));
	gtk_signal_connect_object(GTK_OBJECT(req), "delete_event", GTK_SIGNAL_FUNC(gtk_widget_destroy), GTK_OBJECT(req));
	gtk_signal_connect(GTK_OBJECT(GTK_FILE_SELECTION(req)->ok_button), "clicked", GTK_SIGNAL_FUNC(add_volume_ok), new file_req_assoc(req, NULL));
	gtk_signal_connect_object(GTK_OBJECT(GTK_FILE_SELECTION(req)->cancel_button), "clicked", GTK_SIGNAL_FUNC(gtk_widget_destroy), GTK_OBJECT(req));
	gtk_widget_show(req);
}

// "Create Hardfile" button clicked
static void cb_create_volume(...)
{
	GtkWidget *req = gtk_file_selection_new(GetString(STR_CREATE_VOLUME_TITLE));

	GtkWidget *box = gtk_hbox_new(FALSE, 4);
	gtk_widget_show(box);
	GtkWidget *label = gtk_label_new(GetString(STR_HARDFILE_SIZE_CTRL));
	gtk_widget_show(label);
	GtkWidget *entry = gtk_entry_new();
	gtk_widget_show(entry);
	char str[32];
	sprintf(str, "%d", 40);
	gtk_entry_set_text(GTK_ENTRY(entry), str);
	gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(GTK_FILE_SELECTION(req)->main_vbox), box, FALSE, FALSE, 0);

	gtk_signal_connect_object(GTK_OBJECT(req), "delete_event", GTK_SIGNAL_FUNC(gtk_widget_destroy), GTK_OBJECT(req));
	gtk_signal_connect(GTK_OBJECT(GTK_FILE_SELECTION(req)->ok_button), "clicked", GTK_SIGNAL_FUNC(create_volume_ok), new file_req_assoc(req, entry));
	gtk_signal_connect_object(GTK_OBJECT(GTK_FILE_SELECTION(req)->cancel_button), "clicked", GTK_SIGNAL_FUNC(gtk_widget_destroy), GTK_OBJECT(req));
	gtk_widget_show(req);
}

// "Remove Volume" button clicked
static void cb_remove_volume(...)
{
	GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(volume_list));
	if (sel) {
		GtkTreeIter row;
		if (gtk_tree_selection_get_selected(sel, NULL, &row)) {
			gtk_list_store_remove(volume_list_model, &row);
		}
	}
}

// "Boot From" selected
static void mn_boot_any(...) {PrefsReplaceInt32("bootdriver", 0);}
static void mn_boot_cdrom(...) {PrefsReplaceInt32("bootdriver", CDROMRefNum);}

// "Enable external file system" button toggled
static void tb_enableextfs(GtkWidget *widget)
{
	PrefsReplaceBool("enableextfs", GTK_TOGGLE_BUTTON(widget)->active);
	set_volumes_sensitive();
}

// "No CD-ROM Driver" button toggled
static void tb_nocdrom(GtkWidget *widget)
{
	PrefsReplaceBool("nocdrom", GTK_TOGGLE_BUTTON(widget)->active);
	set_volumes_sensitive();
}

// Add names of CD-ROM devices
static GList *add_cdrom_names(void)
{
	GList *glist = NULL;

	TCHAR rootdir[4] = TEXT("X:\\");
	for (TCHAR letter = TEXT('C'); letter <= TEXT('Z'); letter++) {
		rootdir[0] = letter;
		if (GetDriveType(rootdir) == DRIVE_CDROM)
			glist = g_list_append(glist, _tcsdup(rootdir));
	}

	return glist;
}

// "Enable polling" button toggled
static void tb_pollmedia(GtkWidget *widget)
{
	PrefsReplaceBool("pollmedia", GTK_TOGGLE_BUTTON(widget)->active);
}

// Data source for the volumes list
enum {
	VOLUME_LIST_FILENAME = 0,
	VOLUME_LIST_CDROM,
	VOLUME_LIST_SIZE_DESC,
	NUM_VOLUME_LIST_FIELDS
};

static void init_volume_model() {
	volume_list_model = gtk_list_store_new(NUM_VOLUME_LIST_FIELDS,
		G_TYPE_STRING,	// filename
		G_TYPE_BOOLEAN,	// is CD-ROM
		G_TYPE_STRING	// size desc
		);
}

static void toggle_cdrom_val(GtkTreeIter *row) {
	gboolean cdrom;
	gtk_tree_model_get(GTK_TREE_MODEL(volume_list_model), row,
		VOLUME_LIST_CDROM, &cdrom,
		-1);

	cdrom = !cdrom;

	gtk_list_store_set(volume_list_model, row,
		VOLUME_LIST_CDROM, cdrom,
		-1);
}

// Read settings from widgets and set preferences
static void read_volumes_settings(void)
{
	while (PrefsFindString("disk"))
		PrefsRemoveItem("disk");
	while (PrefsFindString("cdrom"))
		PrefsRemoveItem("cdrom");	

	GtkTreeModel * m = GTK_TREE_MODEL(volume_list_model);

	GtkTreeIter row;
	if (gtk_tree_model_get_iter_first(m, &row)) {
		do {
			GValue filename = G_VALUE_INIT, is_cdrom = G_VALUE_INIT;

			gtk_tree_model_get_value(m, &row, VOLUME_LIST_FILENAME, &filename);
			gtk_tree_model_get_value(m, &row, VOLUME_LIST_CDROM, &is_cdrom);

			//D(bug("handling %d: %s\n", g_value_get_boolean(&is_cdrom), g_value_get_string(&filename)));

			const char * pref_name = g_value_get_boolean(&is_cdrom) ? "cdrom": "disk";
			PrefsAddString(pref_name, g_value_get_string(&filename));

			g_value_unset(&filename);
			g_value_unset(&is_cdrom);

		} while (gtk_tree_model_iter_next(GTK_TREE_MODEL(volume_list_model), &row));
	}

	PrefsReplaceString("extdrives", get_file_entry_path(w_extdrives));

	// Add one more cdrom entry if present in the combo
	const char *str = gtk_entry_get_text(GTK_ENTRY(GTK_COMBO(w_cdrom_drive)->entry));
	if (str && strlen(str) && !volume_in_list(str))
		PrefsAddString("cdrom", str);
}


// Gets the size of the volume as a pretty string
static const char* get_file_size_str (const char * filename)
{
	if (strlen(filename) == 3 && filename[1] == ':' && filename[2] == '\\') {
		// e.g. real CD-ROM drive
		return "";
	}
	std::ifstream in(filename, std::ifstream::ate | std::ifstream::binary);
	if (in.is_open()) {
		uint64_t size = in.tellg();
		in.close();
		char *sizestr = g_format_size_full(size, G_FORMAT_SIZE_IEC_UNITS);
		return sizestr;
	}
	else
	{
		return "Not Found";
	}
}


static bool volume_in_list(const char * filename) {
	bool found = false;

	GtkTreeModel * m = GTK_TREE_MODEL(volume_list_model);

	GtkTreeIter row;
	if (gtk_tree_model_get_iter_first(m, &row)) {
		do {
			GValue cur_filename = G_VALUE_INIT;

			gtk_tree_model_get_value(m, &row, VOLUME_LIST_FILENAME, &cur_filename);

			if (strcmp(g_value_get_string(&cur_filename), filename) == 0) {
				found = true;
			}

			g_value_unset(&cur_filename);

			if (found) break;
		} while (gtk_tree_model_iter_next(GTK_TREE_MODEL(volume_list_model), &row));
	}

	return found;
}


// Add a volume file as the given type
static void add_volume_entry_with_type(const char * filename, bool cdrom) {
	if (volume_in_list(filename)) return;

	GtkTreeIter row;
	gtk_list_store_append(GTK_LIST_STORE(volume_list_model), &row);
	// set the values for the new row
	gtk_list_store_set(GTK_LIST_STORE(volume_list_model), &row,
		VOLUME_LIST_FILENAME, filename,
		VOLUME_LIST_CDROM, cdrom,
		VOLUME_LIST_SIZE_DESC, get_file_size_str(filename),
		-1);
}

static bool has_file_ext (const char * str, const char *ext)
{
	char *file_ext = g_utf8_strrchr(str, 255, '.');
	if (!file_ext)
		return 0;
	return (g_strcmp0(file_ext, ext) == 0);
}

static bool guess_if_file_is_cdrom(const char * volume) {
	return has_file_ext(volume, ".iso") ||
#ifdef BINCUE
		has_file_ext(volume, ".cue") ||
#endif
		has_file_ext(volume, ".toast");
}

// Add a volume file and guess the type
static void add_volume_entry_guessed(const char * filename) {
	add_volume_entry_with_type(filename, guess_if_file_is_cdrom(filename));
}

// CD-ROM checkbox changed
static void cb_cdrom (GtkCellRendererToggle *cell, char *path_str, gpointer data)
{
	GtkTreeIter iter;
	GtkTreePath *path = gtk_tree_path_new_from_string (path_str);
	if (gtk_tree_model_get_iter (GTK_TREE_MODEL(volume_list_model), &iter, path)) {
		toggle_cdrom_val(&iter);
	}
	gtk_tree_path_free (path);
}

static void cb_cdrom_add(...) {
	const char *str = gtk_entry_get_text(GTK_ENTRY(GTK_COMBO(w_cdrom_drive)->entry));
	if (str && strcmp(str, "") != 0) {
		if (volume_in_list(str)) return;
		add_volume_entry_with_type(str, true);

		gtk_entry_set_text(GTK_ENTRY(GTK_COMBO(w_cdrom_drive)->entry), "");
	}
}



// Create "Volumes" pane
static void create_volumes_pane(GtkWidget *top)
{
	GtkWidget *box, *scroll, *menu;
	GtkTreeViewColumn *column;
	GtkCellRenderer *renderer;

	box = make_pane(top, STR_VOLUMES_PANE_TITLE);

	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_widget_show(scroll);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

	init_volume_model();

	volume_list = gtk_tree_view_new();
	gtk_tree_view_set_model(GTK_TREE_VIEW(volume_list), GTK_TREE_MODEL(volume_list_model));
	gtk_widget_show(volume_list);

	gtk_tree_view_set_reorderable(GTK_TREE_VIEW(volume_list), true);

	column = gtk_tree_view_column_new();
	gtk_tree_view_column_set_title(column, GetString(STR_VOL_HEADING_LOCATION));
	gtk_tree_view_column_set_expand(column, true);
	gtk_tree_view_append_column(GTK_TREE_VIEW(volume_list), column);
	renderer = gtk_cell_renderer_text_new();
	gtk_tree_view_column_pack_start(column, renderer, TRUE);
	// connect tree column to model field
	gtk_tree_view_column_add_attribute(column, renderer, "text", VOLUME_LIST_FILENAME);

	column = gtk_tree_view_column_new();
	gtk_tree_view_column_set_title(column, GetString(STR_VOL_HEADING_CDROM));
	gtk_tree_view_append_column(GTK_TREE_VIEW(volume_list), column);
	renderer = gtk_cell_renderer_toggle_new();
	g_signal_connect (renderer, "toggled",
	                  G_CALLBACK (cb_cdrom), NULL);
	gtk_tree_view_column_set_alignment(column, 0.5);

	gtk_tree_view_column_pack_start(column, renderer, TRUE);
	// connect tree column to model field
	gtk_tree_view_column_add_attribute(column, renderer, "active", VOLUME_LIST_CDROM);

	column = gtk_tree_view_column_new();
	gtk_tree_view_column_set_title(column, GetString(STR_VOL_HEADING_SIZE));
	gtk_tree_view_append_column(GTK_TREE_VIEW(volume_list), column);
	renderer = gtk_cell_renderer_text_new();
	gtk_tree_view_column_pack_start(column, renderer, FALSE);
	// connect tree column to model field
	gtk_tree_view_column_add_attribute(column, renderer, "text", VOLUME_LIST_SIZE_DESC);

	GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(volume_list));
	g_signal_connect(sel, "changed", G_CALLBACK(cl_selected), NULL);
	gtk_tree_selection_set_mode(sel, GTK_SELECTION_SINGLE);

	// also support volume files dragged onto the list from outside
	gtk_drag_dest_add_uri_targets(volume_list);
	// add a drop handler to get dropped files; don't supersede the drop handler for reordering
	gtk_signal_connect_after(GTK_OBJECT(volume_list), "drag_data_received", GTK_SIGNAL_FUNC(drag_data_received), NULL);
	// process proposed drops to limit drop locations
	gtk_signal_connect(GTK_OBJECT(volume_list), "drag-motion", GTK_SIGNAL_FUNC(volume_list_drag_motion), NULL);

	char *str;
	int32 index;
	const char * types[] = {"disk", "cdrom", NULL};
	for (const char ** type = types; *type != NULL; type++) {
		index = 0;
		while ((str = const_cast<char *>(PrefsFindString(*type, index))) != NULL) {
			bool is_cdrom = strcmp(*type, "cdrom") == 0;
			add_volume_entry_with_type(str, is_cdrom);
			index++;
		}
	}
	gtk_container_add(GTK_CONTAINER(scroll), volume_list);
	gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

	static const opt_desc buttons[] = {
		{STR_ADD_VOLUME_BUTTON, GTK_SIGNAL_FUNC(cb_add_volume)},
		{STR_CREATE_VOLUME_BUTTON, GTK_SIGNAL_FUNC(cb_create_volume)},
		{STR_REMOVE_VOLUME_BUTTON, G_CALLBACK(cb_remove_volume), &volume_remove_button},
		{0, NULL},
	};
	make_button_box(box, 0, buttons);
	gtk_widget_set_sensitive(volume_remove_button, FALSE);
	make_separator(box);

	static const opt_desc options[] = {
		{STR_BOOT_ANY_LAB, GTK_SIGNAL_FUNC(mn_boot_any)},
		{STR_BOOT_CDROM_LAB, GTK_SIGNAL_FUNC(mn_boot_cdrom)},
		{0, NULL}
	};
	int bootdriver = PrefsFindInt32("bootdriver"), active = 0;
	switch (bootdriver) {
		case 0: active = 0; break;
		case CDROMRefNum: active = 1; break;
	}
	menu = make_option_menu(box, STR_BOOTDRIVER_CTRL, options, active);

	make_checkbox(box, STR_NOCDROM_CTRL, "nocdrom", GTK_SIGNAL_FUNC(tb_nocdrom));

	GList *glist = add_cdrom_names();
	//str = const_cast<char *>(PrefsFindString("cdrom"));
	//if (str == NULL)
		str = "";
	w_cdrom_drive = make_combobox(box, STR_CDROM_DRIVE_CTRL, str, glist);

	GtkWidget * cdrom_hbox = gtk_widget_get_parent(w_cdrom_drive);
	GtkWidget * cdrom_add_button = gtk_button_new_with_label("Add");
		gtk_widget_show(cdrom_add_button);
		gtk_signal_connect_object(GTK_OBJECT(cdrom_add_button), "clicked", GTK_SIGNAL_FUNC(cb_cdrom_add), NULL);
	gtk_box_pack_start(GTK_BOX(cdrom_hbox), cdrom_add_button, FALSE, TRUE, 0);

	make_checkbox(box, STR_POLLMEDIA_CTRL, "pollmedia", GTK_SIGNAL_FUNC(tb_pollmedia));

	make_separator(box);
	w_enableextfs = make_checkbox(box, STR_EXTFS_ENABLE_CTRL, "enableextfs", GTK_SIGNAL_FUNC(tb_enableextfs));
	w_extdrives = make_file_entry(box, STR_EXTFS_DRIVES_CTRL, "extdrives", true);

	set_volumes_sensitive();
}


/*
 *  "JIT Compiler" pane
 */

#ifndef SHEEPSHAVER
static GtkWidget *w_jit_fpu;
static GtkWidget *w_jit_atraps;
static GtkWidget *w_jit_cache_size;
static GtkWidget *w_jit_lazy_flush;
static GtkWidget *w_jit_follow_const_jumps;
#endif

// Set sensitivity of widgets
static void set_jit_sensitive(void)
{
#ifndef SHEEPSHAVER
	const bool jit_enabled = PrefsFindBool("jit");
	gtk_widget_set_sensitive(w_jit_fpu, jit_enabled);
	gtk_widget_set_sensitive(w_jit_cache_size, jit_enabled);
	gtk_widget_set_sensitive(w_jit_lazy_flush, jit_enabled);
	gtk_widget_set_sensitive(w_jit_follow_const_jumps, jit_enabled);
#endif
}

// "Use JIT Compiler" button toggled
static void tb_jit(GtkWidget *widget)
{
	PrefsReplaceBool("jit", GTK_TOGGLE_BUTTON(widget)->active);
	set_jit_sensitive();
}

// "Compile FPU Instructions" button toggled
#ifndef SHEEPSHAVER
static void tb_jit_fpu(GtkWidget *widget)
{
	PrefsReplaceBool("jitfpu", GTK_TOGGLE_BUTTON(widget)->active);
}
#endif

// "Lazy translation cache invalidation" button toggled
#ifndef SHEEPSHAVER
static void tb_jit_lazy_flush(GtkWidget *widget)
{
	PrefsReplaceBool("jitlazyflush", GTK_TOGGLE_BUTTON(widget)->active);
}
#endif

// "Translate through constant jumps (inline blocks)" button toggled
#ifndef SHEEPSHAVER
static void tb_jit_follow_const_jumps(GtkWidget *widget)
{
	PrefsReplaceBool("jitinline", GTK_TOGGLE_BUTTON(widget)->active);
}
#endif

// Read settings from widgets and set preferences
static void read_jit_settings(void)
{
#if USE_JIT
	bool jit_enabled = PrefsFindBool("jit");
	if (jit_enabled) {
#ifndef SHEEPSHAVER
		const char *str = gtk_entry_get_text(GTK_ENTRY(GTK_COMBO(w_jit_cache_size)->entry));
		PrefsReplaceInt32("jitcachesize", atoi(str));
#endif
	}
#endif
}

// "Use built-in 68k DR emulator" button toggled
#ifdef SHEEPSHAVER
static void tb_jit_68k(GtkWidget *widget)
{
	PrefsReplaceBool("jit68k", GTK_TOGGLE_BUTTON(widget)->active);
}
#endif

// Create "JIT Compiler" pane
static void create_jit_pane(GtkWidget *top)
{
#if USE_JIT
	GtkWidget *box, *table, *label, *menu;
	char str[32];
	
	box = make_pane(top, STR_JIT_PANE_TITLE);
	make_checkbox(box, STR_JIT_CTRL, "jit", GTK_SIGNAL_FUNC(tb_jit));
	
#ifndef SHEEPSHAVER
	w_jit_fpu = make_checkbox(box, STR_JIT_FPU_CTRL, "jitfpu", GTK_SIGNAL_FUNC(tb_jit_fpu));
	
	// Translation cache size
	static const combo_desc options[] = {
		STR_JIT_CACHE_SIZE_2MB_LAB,
		STR_JIT_CACHE_SIZE_4MB_LAB,
		STR_JIT_CACHE_SIZE_8MB_LAB,
		STR_JIT_CACHE_SIZE_16MB_LAB,
		0
	};
	sprintf(str, "%d", PrefsFindInt32("jitcachesize"));
	w_jit_cache_size = make_combobox(box, STR_JIT_CACHE_SIZE_CTRL, str, options);
	
	// Lazy translation cache invalidation
	w_jit_lazy_flush = make_checkbox(box, STR_JIT_LAZY_CINV_CTRL, "jitlazyflush", GTK_SIGNAL_FUNC(tb_jit_lazy_flush));

	// Follow constant jumps (inline basic blocks)
	w_jit_follow_const_jumps = make_checkbox(box, STR_JIT_FOLLOW_CONST_JUMPS, "jitinline", GTK_SIGNAL_FUNC(tb_jit_follow_const_jumps));
#endif

	set_jit_sensitive();

#ifdef SHEEPSHAVER
	make_checkbox(box, STR_JIT_68K_CTRL, "jit68k", GTK_SIGNAL_FUNC(tb_jit_68k));
#endif

#endif
}

/*
 *  "SCSI" pane
 */

static GtkWidget *w_scsi[7];

// Read settings from widgets and set preferences
static void read_scsi_settings(void)
{
#ifndef DISABLE_SCSI
	for (int id=0; id<7; id++) {
		char prefs_name[32];
		sprintf(prefs_name, "scsi%d", id);
		const char *str = get_file_entry_path(w_scsi[id]);
		if (str && strlen(str))
			PrefsReplaceString(prefs_name, str);
		else
			PrefsRemoveItem(prefs_name);
	}
#endif
}

// Create "SCSI" pane
static void create_scsi_pane(GtkWidget *top)
{
#ifndef DISABLE_SCSI
	GtkWidget *box;

	box = make_pane(top, STR_SCSI_PANE_TITLE);

	for (int id=0; id<7; id++) {
		char prefs_name[32];
		sprintf(prefs_name, "scsi%d", id);
		w_scsi[id] = make_file_entry(box, STR_SCSI_ID_0 + id, prefs_name);
	}
#endif
}


/*
 *  "Graphics/Sound" pane
 */

// Display types
enum {
	DISPLAY_WINDOW,
	DISPLAY_SCREEN
};

static GtkWidget *w_frameskip, *w_display_x, *w_display_y;
static GtkWidget *l_frameskip, *l_display_x, *l_display_y;
static int display_type;
static int dis_width, dis_height;

// Hide/show graphics widgets
static void hide_show_graphics_widgets(void)
{

}

// "Window" video type selected
static void mn_window(...) {display_type = DISPLAY_WINDOW;}

// "Fullscreen" video type selected
static void mn_fullscreen(...) {display_type = DISPLAY_SCREEN;}

// "5 Hz".."60Hz" selected
static void mn_5hz(...) {PrefsReplaceInt32("frameskip", 12);}
static void mn_7hz(...) {PrefsReplaceInt32("frameskip", 8);}
static void mn_10hz(...) {PrefsReplaceInt32("frameskip", 6);}
static void mn_15hz(...) {PrefsReplaceInt32("frameskip", 4);}
static void mn_30hz(...) {PrefsReplaceInt32("frameskip", 2);}
static void mn_60hz(...) {PrefsReplaceInt32("frameskip", 1);}
static void mn_dynamic(...) {PrefsReplaceInt32("frameskip", 0);}

// QuickDraw acceleration
#ifdef SHEEPSHAVER
static void tb_gfxaccel(GtkWidget *widget)
{
	PrefsReplaceBool("gfxaccel", GTK_TOGGLE_BUTTON(widget)->active);
}
#endif

// Set sensitivity of widgets
static void set_graphics_sensitive(void)
{
	const bool sound_enabled = !PrefsFindBool("nosound");
}

// "Disable Sound Output" button toggled
static void tb_nosound(GtkWidget *widget)
{
	PrefsReplaceBool("nosound", GTK_TOGGLE_BUTTON(widget)->active);
	set_graphics_sensitive();
}

// SDL Graphics
#ifdef USE_SDL_VIDEO
// SDL Renderer Render Driver
enum {
	RENDER_SOFTWARE = 0,
	RENDER_OPENGL = 1,
	RENDER_DIRECT3D = 2
};

GtkWidget *w_render_driver;
GtkWidget *l_render_driver;
static int render_driver;
static int sdl_vsync;

// Render Driver selected
static void mn_sdl_software(...) {render_driver = RENDER_SOFTWARE;}
static void mn_sdl_opengl(...) {render_driver = RENDER_OPENGL;}
static void mn_sdl_direct3d(...) {render_driver = RENDER_DIRECT3D;}

// SDL Renderer Vertical Sync
static void tb_sdl_vsync(GtkWidget *widget)
{
	PrefsReplaceBool("sdl_vsync", GTK_TOGGLE_BUTTON(widget)->active);
}
#endif


// Read graphics preferences
static void parse_graphics_prefs(void)
{
	display_type = DISPLAY_WINDOW;
#ifdef SHEEPSHAVER
	dis_width = 640;
	dis_height = 480;
#else
	dis_width = 512;
	dis_height = 384;
#endif

	const char *str = PrefsFindString("screen");
	if (str) {
		if (sscanf(str, "win/%d/%d", &dis_width, &dis_height) == 2)
			display_type = DISPLAY_WINDOW;
		else if (sscanf(str, "dga/%d/%d", &dis_width, &dis_height) == 2)
			display_type = DISPLAY_SCREEN;
	}

	#ifdef USE_SDL_VIDEO
	render_driver = RENDER_SOFTWARE;

	const char *drv = PrefsFindString("sdlrender");
	if (drv && drv[0]) {
		if (strcmp(drv, "software") == 0)
			render_driver = RENDER_SOFTWARE;
		else if (strcmp(drv, "opengl") == 0)
			render_driver = RENDER_OPENGL;
		else if (strcmp(drv, "direct3d") == 0)
			render_driver = RENDER_DIRECT3D;
	}
	#endif
}

static void read_SDL_graphics_settings(void)
{
	const char *rpref;
	switch (render_driver) {
		case RENDER_SOFTWARE:
			rpref = "software";
			break;
		case RENDER_OPENGL:
			rpref = "opengl";
			break;
		case RENDER_DIRECT3D:
			rpref = "direct3d";
			break;
		default:
			PrefsRemoveItem("sdlrender");
			return;
	}
	PrefsReplaceString("sdlrender", rpref);
}

// Read settings from widgets and set preferences
static void read_graphics_settings(void)
{
	const char *str;

	str = gtk_entry_get_text(GTK_ENTRY(w_display_x));
	dis_width = atoi(str);

	str = gtk_entry_get_text(GTK_ENTRY(w_display_y));
	dis_height = atoi(str);

	char pref[256];
	switch (display_type) {
		case DISPLAY_WINDOW:
			sprintf(pref, "win/%d/%d", dis_width, dis_height);
			break;
		case DISPLAY_SCREEN:
			sprintf(pref, "dga/%d/%d", dis_width, dis_height);
			break;
		default:
			PrefsRemoveItem("screen");
			return;
	}
	PrefsReplaceString("screen", pref);

	#ifdef USE_SDL_VIDEO
	read_SDL_graphics_settings();
	#endif
}

// Create "Graphics/Sound" pane
static void create_graphics_pane(GtkWidget *top)
{
	GtkWidget *box, *table, *label, *opt, *menu, *combo;
	char str[32];

	parse_graphics_prefs();

	box = make_pane(top, STR_GRAPHICS_SOUND_PANE_TITLE);
	table = make_table(box, 2, 5);

	label = gtk_label_new(GetString(STR_VIDEO_TYPE_CTRL));
	gtk_widget_show(label);
	gtk_table_attach(GTK_TABLE(table), label, 0, 1, 0, 1, (GtkAttachOptions)0, (GtkAttachOptions)0, 4, 4);

	opt = gtk_option_menu_new();
	gtk_widget_show(opt);
	menu = gtk_menu_new();
	add_menu_item(menu, STR_WINDOW_LAB, GTK_SIGNAL_FUNC(mn_window));
	add_menu_item(menu, STR_FULLSCREEN_LAB, GTK_SIGNAL_FUNC(mn_fullscreen));
	switch (display_type) {
		case DISPLAY_WINDOW:
			gtk_menu_set_active(GTK_MENU(menu), 0);
			break;
		case DISPLAY_SCREEN:
			gtk_menu_set_active(GTK_MENU(menu), 1);
			break;
	}
	gtk_option_menu_set_menu(GTK_OPTION_MENU(opt), menu);
	gtk_table_attach(GTK_TABLE(table), opt, 1, 2, 0, 1, (GtkAttachOptions)GTK_FILL, (GtkAttachOptions)0, 4, 4);

	l_frameskip = gtk_label_new(GetString(STR_FRAMESKIP_CTRL));
	gtk_widget_show(l_frameskip);
	gtk_table_attach(GTK_TABLE(table), l_frameskip, 0, 1, 1, 2, (GtkAttachOptions)0, (GtkAttachOptions)0, 4, 4);

	w_frameskip = gtk_option_menu_new();
	gtk_widget_show(w_frameskip);
	menu = gtk_menu_new();
	add_menu_item(menu, STR_REF_5HZ_LAB, GTK_SIGNAL_FUNC(mn_5hz));
	add_menu_item(menu, STR_REF_7_5HZ_LAB, GTK_SIGNAL_FUNC(mn_7hz));
	add_menu_item(menu, STR_REF_10HZ_LAB, GTK_SIGNAL_FUNC(mn_10hz));
	add_menu_item(menu, STR_REF_15HZ_LAB, GTK_SIGNAL_FUNC(mn_15hz));
	add_menu_item(menu, STR_REF_30HZ_LAB, GTK_SIGNAL_FUNC(mn_30hz));
	add_menu_item(menu, STR_REF_60HZ_LAB, GTK_SIGNAL_FUNC(mn_60hz));
	add_menu_item(menu, STR_REF_DYNAMIC_LAB, GTK_SIGNAL_FUNC(mn_dynamic));
	int frameskip = PrefsFindInt32("frameskip");
	int item = -1;
	switch (frameskip) {
		case 12: item = 0; break;
		case 8: item = 1; break;
		case 6: item = 2; break;
		case 4: item = 3; break;
		case 2: item = 4; break;
		case 1: item = 5; break;
		case 0: item = 6; break;
	}
	if (item >= 0)
		gtk_menu_set_active(GTK_MENU(menu), item);
	gtk_option_menu_set_menu(GTK_OPTION_MENU(w_frameskip), menu);
	gtk_table_attach(GTK_TABLE(table), w_frameskip, 1, 2, 1, 2, (GtkAttachOptions)GTK_FILL, (GtkAttachOptions)0, 4, 4);

	l_display_x = gtk_label_new(GetString(STR_DISPLAY_X_CTRL));
	gtk_widget_show(l_display_x);
	gtk_table_attach(GTK_TABLE(table), l_display_x, 0, 1, 2, 3, (GtkAttachOptions)0, (GtkAttachOptions)0, 4, 4);

	combo = gtk_combo_new();
	gtk_widget_show(combo);
	GList *glist1 = NULL;
	glist1 = g_list_append(glist1, (void *)GetString(STR_SIZE_512_LAB));
	glist1 = g_list_append(glist1, (void *)GetString(STR_SIZE_640_LAB));
	glist1 = g_list_append(glist1, (void *)GetString(STR_SIZE_800_LAB));
	glist1 = g_list_append(glist1, (void *)GetString(STR_SIZE_1024_LAB));
	glist1 = g_list_append(glist1, (void *)GetString(STR_SIZE_MAX_LAB));
	gtk_combo_set_popdown_strings(GTK_COMBO(combo), glist1);
	if (dis_width)
		sprintf(str, "%d", dis_width);
	else
		strcpy(str, GetString(STR_SIZE_MAX_LAB));
	gtk_entry_set_text(GTK_ENTRY(GTK_COMBO(combo)->entry), str); 
	gtk_table_attach(GTK_TABLE(table), combo, 1, 2, 2, 3, (GtkAttachOptions)GTK_FILL, (GtkAttachOptions)0, 4, 4);
	w_display_x = GTK_COMBO(combo)->entry;

	l_display_y = gtk_label_new(GetString(STR_DISPLAY_Y_CTRL));
	gtk_widget_show(l_display_y);
	gtk_table_attach(GTK_TABLE(table), l_display_y, 0, 1, 3, 4, (GtkAttachOptions)0, (GtkAttachOptions)0, 4, 4);

	combo = gtk_combo_new();
	gtk_widget_show(combo);
	GList *glist2 = NULL;
	glist2 = g_list_append(glist2, (void *)GetString(STR_SIZE_384_LAB));
	glist2 = g_list_append(glist2, (void *)GetString(STR_SIZE_480_LAB));
	glist2 = g_list_append(glist2, (void *)GetString(STR_SIZE_600_LAB));
	glist2 = g_list_append(glist2, (void *)GetString(STR_SIZE_768_LAB));
	glist2 = g_list_append(glist2, (void *)GetString(STR_SIZE_MAX_LAB));
	gtk_combo_set_popdown_strings(GTK_COMBO(combo), glist2);
	if (dis_height)
		sprintf(str, "%d", dis_height);
	else
		strcpy(str, GetString(STR_SIZE_MAX_LAB));
	gtk_entry_set_text(GTK_ENTRY(GTK_COMBO(combo)->entry), str); 
	gtk_table_attach(GTK_TABLE(table), combo, 1, 2, 3, 4, (GtkAttachOptions)GTK_FILL, (GtkAttachOptions)0, 4, 4);
	w_display_y = GTK_COMBO(combo)->entry;

#ifdef SHEEPSHAVER
	make_checkbox(box, STR_GFXACCEL_CTRL, "gfxaccel", GTK_SIGNAL_FUNC(tb_gfxaccel));
#endif

#ifdef USE_SDL_VIDEO
	make_separator(box);

	table = make_table(box, 2, 5);

	l_render_driver = gtk_label_new(GetString(STR_GRAPHICS_SDL_RENDER_DRIVER_CTRL));
	gtk_widget_show(l_render_driver);
	gtk_table_attach(GTK_TABLE(table), l_render_driver, 0, 1, 0, 1, (GtkAttachOptions)0, (GtkAttachOptions)0, 4, 4);

	w_render_driver = gtk_option_menu_new();
	gtk_widget_show(w_render_driver);
	menu = gtk_menu_new();

	add_menu_item(menu, STR_SOFTWARE_LAB, GTK_SIGNAL_FUNC(mn_sdl_software));
	add_menu_item(menu, STR_OPENGL_LAB, GTK_SIGNAL_FUNC(mn_sdl_opengl));
	add_menu_item(menu, STR_DIRECT3D_LAB, GTK_SIGNAL_FUNC(mn_sdl_direct3d));
	switch (render_driver) {
		case RENDER_SOFTWARE:
			gtk_menu_set_active(GTK_MENU(menu), 0);
			break;
		case RENDER_OPENGL:
			gtk_menu_set_active(GTK_MENU(menu), 1);
			break;
		case RENDER_DIRECT3D:
			gtk_menu_set_active(GTK_MENU(menu), 2);
			break;
	}
	gtk_option_menu_set_menu(GTK_OPTION_MENU(w_render_driver), menu);
	gtk_table_attach(GTK_TABLE(table), w_render_driver, 1, 2, 0, 1, (GtkAttachOptions)GTK_FILL, (GtkAttachOptions)0, 4, 4);

	opt = make_checkbox(box, STR_GRAPHICS_SDL_VSYNC_CTRL, "sdl_vsync", GTK_SIGNAL_FUNC(tb_sdl_vsync));
#endif

	make_separator(box);
	make_checkbox(box, STR_NOSOUND_CTRL, "nosound", GTK_SIGNAL_FUNC(tb_nosound));

	set_graphics_sensitive();

	hide_show_graphics_widgets();
}


/*
 *  "Input" pane
 */

static GtkWidget *w_keycode_file;
static GtkWidget *w_mouse_wheel_lines;

// Set sensitivity of widgets
static void set_input_sensitive(void)
{
	const bool use_keycodes = PrefsFindBool("keycodes");
	gtk_widget_set_sensitive(w_keycode_file, use_keycodes);
	gtk_widget_set_sensitive(GTK_WIDGET(g_object_get_data(G_OBJECT(w_keycode_file), "chooser_button")), use_keycodes);
	gtk_widget_set_sensitive(w_mouse_wheel_lines, PrefsFindInt32("mousewheelmode") == 1);
}

// "Use Raw Keycodes" button toggled
static void tb_keycodes(GtkWidget *widget)
{
	PrefsReplaceBool("keycodes", GTK_TOGGLE_BUTTON(widget)->active);
	set_input_sensitive();
}

// "Reserve Windows Key" button toggled
static void tb_reservewindowskey(GtkWidget *widget)
{
	PrefsReplaceBool("reservewindowskey", GTK_TOGGLE_BUTTON(widget)->active);
}

// "Mouse Wheel Mode" selected
static void mn_wheel_page(...) {PrefsReplaceInt32("mousewheelmode", 0); set_input_sensitive();}
static void mn_wheel_cursor(...) {PrefsReplaceInt32("mousewheelmode", 1); set_input_sensitive();}

// Read settings from widgets and set preferences
static void read_input_settings(void)
{
	const char *str = get_file_entry_path(w_keycode_file);
	if (str && strlen(str))
		PrefsReplaceString("keycodefile", str);
	else
		PrefsRemoveItem("keycodefile");

	PrefsReplaceInt32("mousewheellines", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w_mouse_wheel_lines)));
}

// Create "Input" pane
static void create_input_pane(GtkWidget *top)
{
	GtkWidget *box, *hbox, *menu, *label, *button;
	GtkObject *adj;

	box = make_pane(top, STR_INPUT_PANE_TITLE);

	make_checkbox(box, STR_KEYCODES_CTRL, "keycodes", GTK_SIGNAL_FUNC(tb_keycodes));

	hbox = gtk_hbox_new(FALSE, 4);
	gtk_widget_show(hbox);
	gtk_box_pack_start(GTK_BOX(box), hbox, FALSE, FALSE, 0);

	label = gtk_label_new(GetString(STR_KEYCODES_CTRL));
	gtk_widget_show(label);
	gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);

	const char *str = PrefsFindString("keycodefile");
	if (str == NULL)
		str = "";

	w_keycode_file = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(w_keycode_file), str); 
	gtk_widget_show(w_keycode_file);
	gtk_box_pack_start(GTK_BOX(hbox), w_keycode_file, TRUE, TRUE, 0);

	button = make_browse_button(w_keycode_file);
	gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, FALSE, 0);
	g_object_set_data(G_OBJECT(w_keycode_file), "chooser_button", button);

	make_checkbox(box, STR_RESERVE_WINDOWS_KEY_CTRL, "reservewindowskey", GTK_SIGNAL_FUNC(tb_reservewindowskey));

	make_separator(box);

	static const opt_desc options[] = {
		{STR_MOUSEWHEELMODE_PAGE_LAB, GTK_SIGNAL_FUNC(mn_wheel_page)},
		{STR_MOUSEWHEELMODE_CURSOR_LAB, GTK_SIGNAL_FUNC(mn_wheel_cursor)},
		{0, NULL}
	};
	int wheelmode = PrefsFindInt32("mousewheelmode"), active = 0;
	switch (wheelmode) {
		case 0: active = 0; break;
		case 1: active = 1; break;
	}
	menu = make_option_menu(box, STR_MOUSEWHEELMODE_CTRL, options, active);

	hbox = gtk_hbox_new(FALSE, 4);
	gtk_widget_show(hbox);
	gtk_box_pack_start(GTK_BOX(box), hbox, FALSE, FALSE, 0);

	label = gtk_label_new(GetString(STR_MOUSEWHEELLINES_CTRL));
	gtk_widget_show(label);
	gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);

	adj = gtk_adjustment_new(PrefsFindInt32("mousewheellines"), 1, 1000, 1, 5, 0);
	w_mouse_wheel_lines = gtk_spin_button_new(GTK_ADJUSTMENT(adj), 0.0, 0);
	gtk_widget_show(w_mouse_wheel_lines);
	gtk_box_pack_start(GTK_BOX(hbox), w_mouse_wheel_lines, FALSE, FALSE, 0);

	set_input_sensitive();
}


/*
 *  "Serial" pane
 */

static GtkWidget *w_seriala, *w_portfile0;
static GtkWidget *w_serialb, *w_portfile1;

// Set sensitivity of widgets
static void set_serial_sensitive(void)
{
	const char *str;
	bool is_file;

	str = gtk_entry_get_text(GTK_ENTRY(w_seriala));
	is_file = strcmp(str, "FILE") == 0;
	gtk_widget_set_sensitive(w_portfile0, is_file);
	gtk_widget_set_sensitive(GTK_WIDGET(g_object_get_data(G_OBJECT(w_portfile0), "chooser_button")), is_file);

	str = gtk_entry_get_text(GTK_ENTRY(w_serialb));
	is_file = strcmp(str, "FILE") == 0;
	gtk_widget_set_sensitive(w_portfile1, is_file);
	gtk_widget_set_sensitive(GTK_WIDGET(g_object_get_data(G_OBJECT(w_portfile1), "chooser_button")), is_file);
}

// Read settings from widgets and set preferences
static void read_serial_settings(void)
{
	const char *str;

	str = gtk_entry_get_text(GTK_ENTRY(w_seriala));
	PrefsReplaceString("seriala", str);

	str = gtk_entry_get_text(GTK_ENTRY(w_serialb));
	PrefsReplaceString("serialb", str);

	str = gtk_entry_get_text(GTK_ENTRY(w_portfile0));
	PrefsReplaceString("portfile0", str);

	str = gtk_entry_get_text(GTK_ENTRY(w_portfile1));
	PrefsReplaceString("portfile1", str);
}

// Port changed in combo
static void cb_serial_port_changed(...)
{
	set_serial_sensitive();
}

// Add names of serial devices
static GList *add_serial_names(void)
{
	GList *glist = NULL;

	static const char *port_names[] = {
		"COM1", "COM2", "COM3", "COM4", "COM5", "COM6",
		"LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6",
		"FILE",
		NULL
	};

	for (int i = 0; port_names[i] != NULL; i++)
		glist = g_list_append(glist, (void *)port_names[i]);

	return glist;
}

// Create "Serial" pane
static void create_serial_pane(GtkWidget *top)
{
	GtkWidget *box, *hbox, *table, *label, *combo, *sep, *entry;
	GtkObject *adj;

	box = make_pane(top, STR_SERIAL_PANE_TITLE);
	table = make_table(box, 2, 5);

	GList *glist = add_serial_names();
	const char *str = PrefsFindString("seriala");
	combo = table_make_combobox(table, 0, STR_SERIALA_CTRL, str, glist);
	w_seriala = GTK_COMBO(combo)->entry;
	gtk_signal_connect(GTK_OBJECT(w_seriala), "changed", GTK_SIGNAL_FUNC(cb_serial_port_changed), NULL);

	w_portfile0 = table_make_file_entry(table, 1, STR_FILE_CTRL, "portfile0");

	sep = gtk_hseparator_new();
	gtk_widget_show(sep);
	gtk_table_attach(GTK_TABLE(table), sep, 0, 2, 2, 3, (GtkAttachOptions)0, (GtkAttachOptions)0, 4, 4);

	str = PrefsFindString("serialb");
	combo = table_make_combobox(table, 3, STR_SERIALB_CTRL, str, glist);
	w_serialb = GTK_COMBO(combo)->entry;
	gtk_signal_connect(GTK_OBJECT(w_serialb), "changed", GTK_SIGNAL_FUNC(cb_serial_port_changed), NULL);

	w_portfile1 = table_make_file_entry(table, 4, STR_FILE_CTRL, "portfile1");

	set_serial_sensitive();
}


/*
 *  "Ethernet" pane
 */

static GtkWidget *w_ftp_port_list, *w_tcp_port_list;

// Set sensitivity of widgets
static void set_ethernet_sensitive(void)
{
	const char *str = PrefsFindString("ether");

	bool is_router = str && strcmp(str, "router") == 0;
	gtk_widget_set_sensitive(w_ftp_port_list, is_router);
	gtk_widget_set_sensitive(w_tcp_port_list, is_router);
}

// Read settings from widgets and set preferences
static void read_ethernet_settings(void)
{
	const char *str = PrefsFindString("ether");

	bool is_router = str && strcmp(str, "router") == 0;
	if (is_router) {
		str = gtk_entry_get_text(GTK_ENTRY(w_ftp_port_list));
		PrefsReplaceString("ftp_port_list", str);
		str = gtk_entry_get_text(GTK_ENTRY(w_tcp_port_list));
		PrefsReplaceString("tcp_port", str);
	}
}

// Ethernet emulation type changed in menulist
static void cb_ether_changed(...)
{
	set_ethernet_sensitive();
}

// Ethernet option "None" selected
static void mn_ether_none(void)
{
	PrefsRemoveItem("ether");
	PrefsRemoveItem("etherguid");
}

// Ethernet option "Basilisk II Router" selected
static void mn_ether_router(void)
{
	PrefsReplaceString("ether", "router");
	PrefsRemoveItem("etherguid");
}

// Ethernet option "Basilisk II Slirp" selected
static void mn_ether_slirp(void)
{
	PrefsReplaceString("ether", "slirp");
	PrefsRemoveItem("etherguid");
}

// Ethernet option for Basilisk II driver selected
static void mn_ether_b2ether(GtkWidget *, const char *guid)
{
	PrefsReplaceString("ether", "b2ether");
	PrefsReplaceString("etherguid", guid);
}

// Ethernet option for Basilisk II driver selected
static void mn_ether_tap(GtkWidget *, const char *guid)
{
	PrefsReplaceString("ether", "tap");
	PrefsReplaceString("etherguid", guid);
}

// Create ethernet interfaces menu
static int create_ether_menu(GtkWidget *menu)
{
	int active = -1;
	int n_items = 0;
	const char *ether = PrefsFindString("ether");
	const char *etherguid = PrefsFindString("etherguid");

	// No Ethernet
	add_menu_item(menu, STR_NONE_LAB, (GtkSignalFunc)mn_ether_none);
	if (ether == NULL)
		active = n_items;
	n_items++;

	// Basilisk II Router
	add_menu_item(menu, "Basilisk II Router", (GtkSignalFunc)mn_ether_router);
	if (ether && strcmp(ether, "router") == 0)
		active = n_items;
	n_items++;

	// Basilisk II Slirp
	add_menu_item(menu, "Basilisk II Slirp", (GtkSignalFunc)mn_ether_slirp);
	if (ether && strcmp(ether, "slirp") == 0)
		active = n_items;
	n_items++;

	// Basilisk II Ethernet Adapter
	PacketOpenAdapter(TEXT(""), 0);
	{
		ULONG sz;
		TCHAR names[1024];
		sz = sizeof(names);
		if (PacketGetAdapterNames(NULL, names, &sz) == ERROR_SUCCESS) {
			TCHAR *p = names;
			while (*p) {
				const TCHAR DEVICE_HEADER[] = TEXT("\\Device\\B2ether_");
				if (_tcsnicmp(p, DEVICE_HEADER, sizeof(DEVICE_HEADER) - 1) == 0) {
					LPADAPTER fd = PacketOpenAdapter(p + sizeof(DEVICE_HEADER) - 1, 0);
					if (fd) {
						TCHAR guid[256];
						_stprintf(guid, TEXT("%s"), p + sizeof(DEVICE_HEADER) - 1);
						const gchar *name = tchar_to_g_utf8(ether_guid_to_name(guid));
						if (name) {
							std::string str_guid = to_string(guid);
							add_menu_item(menu, name, (GtkSignalFunc)mn_ether_b2ether, strdup(str_guid.c_str()));
							if (etherguid && to_tstring(guid).compare(to_tstring(etherguid)) == 0 &&
								ether && strcmp(ether, "b2ether") == 0)
								active = n_items;
							n_items++;
						}
						PacketCloseAdapter(fd);
					}
				}
				p += _tcslen(p) + 1;
			}
		}
	}
	PacketCloseAdapter(NULL);

	// TAP-Win32
	const TCHAR *tap_devices;
	if ((tap_devices = ether_tap_devices()) != NULL) {
		const TCHAR *guid = tap_devices;
		while (*guid) {
			const gchar *name = tchar_to_g_utf8(ether_guid_to_name(guid));
			if (name) {
				std::string str_guid = to_string(guid);
				add_menu_item(menu, name, (GtkSignalFunc)mn_ether_tap, strdup(str_guid.c_str()));
				if (etherguid && to_tstring(guid).compare(to_tstring(etherguid)) == 0 &&
					ether && strcmp(ether, "tap") == 0)
					active = n_items;
				n_items++;
			}
			guid += _tcslen(guid) + 1;
		}
		free((char *)tap_devices);
	}

	return active;
}

// Create "Ethernet" pane
static void create_ethernet_pane(GtkWidget *top)
{
	GtkWidget *box, *hbox, *table, *label, *sep, *entry, *opt, *menu, *item;

	box = make_pane(top, STR_NETWORK_PANE_TITLE);
	table = make_table(box, 2, 5);

	label = gtk_label_new(GetString(STR_ETHERNET_IF_CTRL));
	gtk_widget_show(label);
	gtk_table_attach(GTK_TABLE(table), label, 0, 1, 0, 1, (GtkAttachOptions)0, (GtkAttachOptions)0, 4, 4);

	opt = gtk_option_menu_new();
	gtk_widget_show(opt);
	menu = gtk_menu_new();
	int active = create_ether_menu(menu);
	if (active >= 0)
		gtk_menu_set_active(GTK_MENU(menu), active);
	gtk_option_menu_set_menu(GTK_OPTION_MENU(opt), menu);
	gtk_table_attach(GTK_TABLE(table), opt, 1, 2, 0, 1, (GtkAttachOptions)(GTK_FILL | GTK_EXPAND), (GtkAttachOptions)0, 4, 4);
	gtk_signal_connect(GTK_OBJECT(opt), "changed", GTK_SIGNAL_FUNC(cb_ether_changed), NULL);

	sep = gtk_hseparator_new();
	gtk_widget_show(sep);
	gtk_table_attach(GTK_TABLE(table), sep, 0, 2, 1, 2, (GtkAttachOptions)0, (GtkAttachOptions)0, 4, 4);

	label = gtk_label_new(GetString(STR_ETHER_FTP_PORT_LIST_CTRL));
	gtk_widget_show(label);
	gtk_table_attach(GTK_TABLE(table), label, 0, 1, 2, 3, (GtkAttachOptions)0, (GtkAttachOptions)0, 4, 4);

	entry = gtk_entry_new();
	const char *str = PrefsFindString("ftp_port_list");
	if (str == NULL)
		str = "";
	gtk_entry_set_text(GTK_ENTRY(entry), str);
	gtk_widget_show(entry);
	gtk_table_attach(GTK_TABLE(table), entry, 1, 2, 2, 3, (GtkAttachOptions)(GTK_FILL | GTK_EXPAND), (GtkAttachOptions)0, 4, 4);
	w_ftp_port_list = entry;

	label = gtk_label_new(GetString(STR_ETHER_TCP_PORT_LIST_CTRL));
	gtk_widget_show(label);
	gtk_table_attach(GTK_TABLE(table), label, 0, 1, 3, 4, (GtkAttachOptions)0, (GtkAttachOptions)0, 4, 4);

	entry = gtk_entry_new();
	str = PrefsFindString("tcp_port");
	if (str == NULL)
		str = "";
	gtk_entry_set_text(GTK_ENTRY(entry), str);
	gtk_widget_show(entry);
	gtk_table_attach(GTK_TABLE(table), entry, 1, 2, 3, 4, (GtkAttachOptions)(GTK_FILL | GTK_EXPAND), (GtkAttachOptions)0, 4, 4);
	w_tcp_port_list = entry;

	set_ethernet_sensitive();
}


/*
 *  "Memory/Misc" pane
 */

static GtkWidget *w_ramsize;
static GtkWidget *w_rom_file;

// Don't use CPU when idle?
static void tb_idlewait(GtkWidget *widget)
{
	PrefsReplaceBool("idlewait", GTK_TOGGLE_BUTTON(widget)->active);
}

// "Ignore SEGV" button toggled
#ifdef HAVE_SIGSEGV_SKIP_INSTRUCTION
static void tb_ignoresegv(GtkWidget *widget)
{
	PrefsReplaceBool("ignoresegv", GTK_TOGGLE_BUTTON(widget)->active);
}
#endif

// Model ID selected
static void mn_modelid_5(...) {PrefsReplaceInt32("modelid", 5);}
static void mn_modelid_14(...) {PrefsReplaceInt32("modelid", 14);}

// CPU/FPU type
static void mn_cpu_68020(...) {PrefsReplaceInt32("cpu", 2); PrefsReplaceBool("fpu", false);}
static void mn_cpu_68020_fpu(...) {PrefsReplaceInt32("cpu", 2); PrefsReplaceBool("fpu", true);}
static void mn_cpu_68030(...) {PrefsReplaceInt32("cpu", 3); PrefsReplaceBool("fpu", false);}
static void mn_cpu_68030_fpu(...) {PrefsReplaceInt32("cpu", 3); PrefsReplaceBool("fpu", true);}
static void mn_cpu_68040(...) {PrefsReplaceInt32("cpu", 4); PrefsReplaceBool("fpu", true);}

// Read settings from widgets and set preferences
static void read_memory_settings(void)
{
	const char *str = gtk_entry_get_text(GTK_ENTRY(GTK_COMBO(w_ramsize)->entry));
	PrefsReplaceInt32("ramsize", atoi(str) << 20);

	str = get_file_entry_path(w_rom_file);
	if (str && strlen(str))
		PrefsReplaceString("rom", str);
	else
		PrefsRemoveItem("rom");

}

// Create "Memory/Misc" pane
static void create_memory_pane(GtkWidget *top)
{
	GtkWidget *box, *hbox, *table, *label, *scale, *menu;

	box = make_pane(top, STR_MEMORY_MISC_PANE_TITLE);
	table = make_table(box, 2, 5);

	static const combo_desc options[] = {
#ifndef SHEEPSHAVER
		STR_RAMSIZE_2MB_LAB,
#endif
		STR_RAMSIZE_4MB_LAB,
		STR_RAMSIZE_8MB_LAB,
		STR_RAMSIZE_16MB_LAB,
		STR_RAMSIZE_32MB_LAB,
		STR_RAMSIZE_64MB_LAB,
		STR_RAMSIZE_128MB_LAB,
		STR_RAMSIZE_256MB_LAB,
		STR_RAMSIZE_512MB_LAB,
		STR_RAMSIZE_1024MB_LAB,
		0
	};
	char default_ramsize[16];
	sprintf(default_ramsize, "%d", PrefsFindInt32("ramsize") >> 20);
	w_ramsize = table_make_combobox(table, 0, STR_RAMSIZE_CTRL, default_ramsize, options);

#ifndef SHEEPSHAVER
	static const opt_desc model_options[] = {
		{STR_MODELID_5_LAB, GTK_SIGNAL_FUNC(mn_modelid_5)},
		{STR_MODELID_14_LAB, GTK_SIGNAL_FUNC(mn_modelid_14)},
		{0, NULL}
	};
	int modelid = PrefsFindInt32("modelid"), active = 0;
	switch (modelid) {
		case 5: active = 0; break;
		case 14: active = 1; break;
	}
	table_make_option_menu(table, 2, STR_MODELID_CTRL, model_options, active);
#endif

#if EMULATED_68K
	static const opt_desc cpu_options[] = {
		{STR_CPU_68020_LAB, GTK_SIGNAL_FUNC(mn_cpu_68020)},
		{STR_CPU_68020_FPU_LAB, GTK_SIGNAL_FUNC(mn_cpu_68020_fpu)},
		{STR_CPU_68030_LAB, GTK_SIGNAL_FUNC(mn_cpu_68030)},
		{STR_CPU_68030_FPU_LAB, GTK_SIGNAL_FUNC(mn_cpu_68030_fpu)},
		{STR_CPU_68040_LAB, GTK_SIGNAL_FUNC(mn_cpu_68040)},
		{0, NULL}
	};
	int cpu = PrefsFindInt32("cpu");
	bool fpu = PrefsFindBool("fpu");
	active = 0;
	switch (cpu) {
		case 2: active = fpu ? 1 : 0; break;
		case 3: active = fpu ? 3 : 2; break;
		case 4: active = 4;
	}
	table_make_option_menu(table, 3, STR_CPU_CTRL, cpu_options, active);
#endif

	w_rom_file = table_make_file_entry(table, 4, STR_ROM_FILE_CTRL, "rom");

	make_checkbox(box, STR_IDLEWAIT_CTRL, "idlewait", GTK_SIGNAL_FUNC(tb_idlewait));

#ifdef HAVE_SIGSEGV_SKIP_INSTRUCTION
	make_checkbox(box, STR_IGNORESEGV_CTRL, "ignoresegv", GTK_SIGNAL_FUNC(tb_ignoresegv));
#endif
}


/*
 *  Read settings from widgets and set preferences
 */

static void read_settings(void)
{
	read_volumes_settings();
	read_scsi_settings();
	read_graphics_settings();
	read_input_settings();
	read_serial_settings();
	read_ethernet_settings();
	read_memory_settings();
	read_jit_settings();
}


/*
 *  Fake unused data and functions
 */

uint8 XPRAM[XPRAM_SIZE];
void MountVolume(void *fh) { }
void FileDiskLayout(loff_t size, uint8 *data, loff_t &start_byte, loff_t &real_size) { }
void recycle_write_packet(LPPACKET) { }
VOID CALLBACK packet_read_completion(DWORD, DWORD, LPOVERLAPPED) { }


/*
 *  Add default serial prefs (must be added, even if no ports present)
 */

void SysAddSerialPrefs(void)
{
	PrefsAddString("seriala", "COM1");
	PrefsAddString("serialb", "COM2");
}


/*
 *  Display alerts
 */

static HWND GetMainWindowHandle() {
	return NULL;
}

static void display_alert(int title_id, const char *text, int flags)
{
	HWND hMainWnd = GetMainWindowHandle();
	MessageBoxA(hMainWnd, text, GetString(title_id), MB_OK | flags);
}
#ifdef _UNICODE
static void display_alert(int title_id, const wchar_t *text, int flags)
{
	HWND hMainWnd = GetMainWindowHandle();
	MessageBoxW(hMainWnd, text, GetStringW(title_id).get(), MB_OK | flags);
}
#endif


/*
 *  Display error alert
 */

void ErrorAlert(const char *text)
{
	if (PrefsFindBool("nogui"))
		return;

	display_alert(STR_ERROR_ALERT_TITLE, text, MB_ICONSTOP);
}
#ifdef _UNICODE
void ErrorAlert(const wchar_t *text)
{
	if (PrefsFindBool("nogui"))
		return;

	display_alert(STR_ERROR_ALERT_TITLE, text, MB_ICONSTOP);
}
#endif


/*
 *  Display warning alert
 */

void WarningAlert(const char *text)
{
	if (PrefsFindBool("nogui"))
		return;

	display_alert(STR_WARNING_ALERT_TITLE, text, MB_ICONSTOP);
}
#ifdef _UNICODE
void WarningAlert(const wchar_t *text)
{
	if (PrefsFindBool("nogui"))
		return;

	display_alert(STR_WARNING_ALERT_TITLE, text, MB_ICONSTOP);
}
#endif

/*
 *  Start standalone GUI
 */

int main(int argc, char *argv[])
{
	// Init GTK
	gtk_set_locale();
	gtk_init(&argc, &argv);

	// Read preferences
	PrefsInit(NULL, argc, argv);

	// Migrate preferences
	PrefsMigrate();

	// Show preferences editor
	bool start = PrefsEditor();

	// Exit preferences
	PrefsExit();

	// Transfer control to the executable
	if (start) {
		TCHAR path[_MAX_PATH];
		bool ok = GetModuleFileName(NULL, path, sizeof(path)) != 0;
		if (ok) {
			TCHAR b2_path[_MAX_PATH];
			TCHAR *p = _tcsrchr(path, TEXT('\\'));
			*++p = TEXT('\0');
			SetCurrentDirectory(path);
			_tcscpy(b2_path, path);
			_tcscat(b2_path, TEXT(PROGRAM_NAME));
			_tcscat(b2_path, TEXT(".exe"));
			HINSTANCE h = ShellExecute(GetDesktopWindow(), TEXT("open"),
									   b2_path, TEXT(""), path, SW_SHOWNORMAL);
			if ((int)h <= 32)
				ok = false;
		}
		if (!ok) {
			ErrorAlert(TEXT("Could not start ") TEXT(PROGRAM_NAME) TEXT(" executable"));
			return 1;
		}
	}

	return 0;
}
