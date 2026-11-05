/* volctl - tray-style volume control for the Galileo board
 *  - 28x28 undecorated speaker icon docked at the bottom-right
 *  - click it: popup with a draggable slider + card selector (USB/HDMI)
 * Talks to ALSA through amixer(1); no pulseaudio involved.
 */
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CONF "/etc/asound.conf"

static int card = 1;
static char ctl[32] = "PCM";
static GtkWidget *popup, *scale, *combo, *icon;
static int updating;

static int sh(const char *cmd, char *out, size_t n)
{
	FILE *f = popen(cmd, "r");
	if (!f) return -1;
	size_t got = fread(out, 1, n - 1, f);
	out[got] = 0;
	pclose(f);
	return (int)got;
}

static int vol_get(void)
{
	char buf[4096], cmd[128], *p, *q;
	snprintf(cmd, sizeof cmd, "amixer -c %d sget %s 2>/dev/null", card, ctl);
	if (sh(cmd, buf, sizeof buf) <= 0) return -1;
	for (p = buf; (p = strchr(p, '[')); p++) {
		int v;
		if (sscanf(p, "[%d%%]", &v) == 1) return v;
		if ((q = strchr(p, ']')) == NULL) break;
		p = q;
	}
	return -1;
}

static void vol_set(int pct)
{
	char cmd[160];
	snprintf(cmd, sizeof cmd, "amixer -c %d sset %s %d%% >/dev/null 2>&1", card, ctl, pct);
	if (system(cmd)) { /* ignore */ }
}

static void card_write(int c)
{
	FILE *f = fopen(CONF, "w");
	if (!f) return;
	fprintf(f,
		"pcm.!default {\n    type plug\n    slave.pcm { type hw; card %d; device %d }\n}\n"
		"ctl.!default { type hw; card %d }\n",
		c, c == 0 ? 3 : 0, c);
	fclose(f);
}

static void pick_ctl(void)
{
	char cmd[128], buf[2048];
	const char *tries[] = { "Master", "PCM", "Speaker", "Headphone", NULL };
	for (int i = 0; tries[i]; i++) {
		snprintf(cmd, sizeof cmd, "amixer -c %d sget %s 2>/dev/null", card, tries[i]);
		if (sh(cmd, buf, sizeof buf) > 0 && strstr(buf, "Playback")) {
			snprintf(ctl, sizeof ctl, "%s", tries[i]);
			return;
		}
	}
}

/* speaker glyph drawn with cairo: box + cone + one arc */
static gboolean icon_draw(GtkWidget *w, GdkEventExpose *e, gpointer d)
{
	cairo_t *cr = gdk_cairo_create(gtk_widget_get_window(w));
	double h = w->allocation.height;
	cairo_set_source_rgb(cr, 0.85, 0.9, 1.0);
	cairo_rectangle(cr, 4, h/2 - 4, 5, 8); cairo_fill(cr);
	cairo_move_to(cr, 9, h/2 - 4); cairo_line_to(cr, 17, h/2 - 9);
	cairo_line_to(cr, 17, h/2 + 9); cairo_line_to(cr, 9, h/2 + 4);
	cairo_close_path(cr); cairo_fill(cr);
	cairo_set_line_width(cr, 2); cairo_arc(cr, 17, h/2, 6, -0.9, 0.9); cairo_stroke(cr);
	cairo_destroy(cr);
	return FALSE;
}

static void on_scale(GtkRange *r, gpointer d)
{
	if (updating) return;
	int v = (int)gtk_range_get_value(r);
	vol_set(v);
}

static void on_card(GtkComboBox *c, gpointer d)
{
	int idx = gtk_combo_box_get_active(c);
	card = idx == 0 ? 1 : 0;
	card_write(card);
	pick_ctl();
	int v = vol_get();
	updating = 1;
	gtk_range_set_value(GTK_RANGE(scale), v < 0 ? 0 : v);
	updating = 0;
}

static int popup_shown;

/* Hiding a mapped toplevel makes the WM restack windows, which then buries
 * the override-redirect icon window under lxpanel: keep the dialog mapped and
 * move it off-screen instead. */
static void toggle(GtkWidget *w, gpointer d)
{
	(void)w; (void)d;
	if (popup_shown) {
		gtk_window_move(GTK_WINDOW(popup), 30000, 30000);
		popup_shown = 0;
	} else {
		int v = vol_get();
		updating = 1;
		gtk_range_set_value(GTK_RANGE(scale), v < 0 ? 0 : v);
		gtk_combo_box_set_active(GTK_COMBO_BOX(combo), card == 1 ? 0 : 1);
		updating = 0;
		gtk_window_move(GTK_WINDOW(popup), 1000, 1024 - 220);
		gtk_window_present(GTK_WINDOW(popup));
		popup_shown = 1;
	}
	/* keep the icon on top of the panel after any restack */
	if (gtk_widget_get_window(icon))
		gdk_window_raise(gtk_widget_get_window(icon));
}

static gboolean on_icon_click(GtkWidget *w, GdkEventButton *b, gpointer d)
{
	(void)b; (void)d;
	toggle(w, d);
	return TRUE;
}

int main(int argc, char **argv)
{
	gtk_init(&argc, &argv);
	card_write(card);
	pick_ctl();

	icon = gtk_window_new(GTK_WINDOW_POPUP);
	gtk_window_set_default_size(GTK_WINDOW(icon), 24, 24);
	gtk_window_set_keep_above(GTK_WINDOW(icon), TRUE);
	gtk_window_set_skip_taskbar_hint(GTK_WINDOW(icon), TRUE);
	GtkWidget *da = gtk_drawing_area_new();
	gtk_widget_set_size_request(da, 24, 24);
	gtk_widget_modify_bg(da, GTK_STATE_NORMAL,
		&(GdkColor){ 0, 0x2a2a, 0x2a2a, 0x2a2a });
	gtk_container_add(GTK_CONTAINER(icon), da);
	/* a GtkDrawingArea receives no input events unless the mask is added */
	gtk_widget_add_events(da, GDK_BUTTON_PRESS_MASK);
	g_signal_connect(da, "button-press-event", G_CALLBACK(on_icon_click), NULL);
	g_signal_connect(da, "expose-event", G_CALLBACK(icon_draw), NULL);
	/* sit inside the 28px taskbar strip, left of the CPU graph */
	gtk_window_move(GTK_WINDOW(icon), 1146, 1000);
	gtk_widget_show_all(icon);

	popup = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(popup), "Volume");
	gtk_window_set_decorated(GTK_WINDOW(popup), TRUE);
	gtk_window_set_keep_above(GTK_WINDOW(popup), TRUE);
	gtk_window_set_skip_taskbar_hint(GTK_WINDOW(popup), TRUE);
	gtk_window_set_position(GTK_WINDOW(popup), GTK_WIN_POS_NONE);
	GtkWidget *vb = gtk_vbox_new(FALSE, 6);
	GtkWidget *lbl = gtk_label_new("Volume");
	gtk_box_pack_start(GTK_BOX(vb), lbl, FALSE, FALSE, 0);
	scale = gtk_hscale_new_with_range(0, 100, 1);
	gtk_widget_set_size_request(scale, 220, -1);
	g_signal_connect(scale, "value-changed", G_CALLBACK(on_scale), NULL);
	gtk_box_pack_start(GTK_BOX(vb), scale, FALSE, FALSE, 0);
	combo = gtk_combo_box_text_new();
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "USB Audio (card1)");
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "HDMI (card0)");
	gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
	g_signal_connect(combo, "changed", G_CALLBACK(on_card), NULL);
	gtk_box_pack_start(GTK_BOX(vb), combo, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(popup), vb);
	gtk_widget_show_all(popup);
	gtk_window_move(GTK_WINDOW(popup), 30000, 30000);
	if (argc > 1 && strcmp(argv[1], "--show") == 0) {
		int v = vol_get();
		updating = 1;
		gtk_range_set_value(GTK_RANGE(scale), v < 0 ? 0 : v);
		updating = 0;
		gtk_window_move(GTK_WINDOW(popup), 1000, 1024 - 220);
		popup_shown = 1;
	}

	gtk_main();
	return 0;
}
