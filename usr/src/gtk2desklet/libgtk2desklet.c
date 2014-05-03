//gcc -g -fPIC -shared $SRCNAME -o ${OBJNAME}.so `pkg-config --cflags --libs gtk+-2.0` && ./install

//gtk2desklet: Convert GTK programs into desktop apps
//This is a GTK module, after compiling place it at /usr/lib/gtk-2.0/modules/

/*
 * Copyright 2012 Akash Rawal

 * This file is part of gtk2desklet.
  
 * gtk2desklet is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * gtk2desklet is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with gtk2desklet.  If not, see <http://www.gnu.org/licenses/>.
 */
///////////////////////////////////////////////////////////////////////////////

#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gtk2desklet"

///////////////////////////////////////////////////////////////////////////////

//Gets current wallpaper
Pixmap x_get_root_pixmap_by_property(Display *dpy)
{
	Pixmap root_pixmap = None;
	//Properties that may store wallpaper
	const char* pixmap_id_names[] =
	{
		"_XROOTPMAP_ID", "ESETROOT_PMAP_ID", NULL
	};
	int i, total;
	static Atom ids[4];
	static gboolean setup_done = FALSE;
	if (! setup_done) 
	{
		for (total = 0; pixmap_id_names[total]; total++);
		XInternAtoms(dpy, (char **) pixmap_id_names, total, True, ids);
		setup_done = TRUE;
	}

	for (i=0; (pixmap_id_names[i] && (root_pixmap == None)); i++)
	{
		if (ids[i] != None)
		{
			Atom actual_type = None;
			int actual_format = 0;
			unsigned long nitems = 0;
			unsigned long bytes_after = 0;
			unsigned char *properties = NULL;
			int rc = 0;

			rc = XGetWindowProperty(dpy, DefaultRootWindow(dpy), ids[i], 0, 1,
					False, XA_PIXMAP, &actual_type, &actual_format, &nitems,
					&bytes_after, &properties);

			if (rc == Success && properties)
			{
				root_pixmap = *((Pixmap*)properties);
			}
		}
	}

	return root_pixmap;
}
//Check whether window has been moved
gboolean check_window_moved(GdkWindow *win)
{
	static int ox = 0, oy = 0, ow = 0, oh = 0;
	int nx, ny, nw, nh;
	
	gdk_window_get_geometry(win, &nx, &ny, &nw, &nh, NULL);
	
	if ( (nx != ox) || (ny != oy) || (nw != ow) || (nh != oh) )
	{
		ox = nx;
		oy = ny;
		ow = nw;
		oh = nh;
		return TRUE;
	}
	else
		return FALSE;
}
//Maintain list of GCs
typedef struct
{
	int depth;
	GC gc;
} GCListElement;
typedef struct
{
	GSList *list;
	Display *disp;
	XGCValues gcv;
	gulong mask;
} GCList;
GC gc_list_get_gc(GCList *me, Drawable d, int depth)
{
	GSList *iter;
	GCListElement *new_element;
	//Search for available GCs with given depth
	for (iter = me->list; iter != NULL; iter=iter->next)
		if (((GCListElement *)(iter->data))->depth == depth)
			return ((GCListElement *)(iter->data))->gc;
	
	//GC with required depth not found, create a new GC
	new_element = g_slice_new(GCListElement);
	new_element->depth = depth;
	new_element->gc = XCreateGC(me->disp, d, me->mask, &(me->gcv));
	me->list = g_slist_prepend(me->list, new_element);
	return new_element->gc;
}

///////////////////////////////////////////////////////////////////////////////
//Data storage structure

typedef struct
{
	//Program options
	gboolean transparent;
	GdkWindow *win;
	GtkWidget *wid;
	Display *disp;
	Screen *scr;
	gboolean is_multithreaded;
	
	//Transparency-related data
	Window rootwin;
	GCList bg_gc_list;
	
	GdkPixmap *bgpix;     //
	GdkPixmap *trans_bg;
	GdkPixmap *pseudo_bg;
} Desklet;

//v2.3: Take care of multithreaded programs
void gd_lock_gdk(Desklet *me)
{
	if (me->is_multithreaded)
	{
		gdk_threads_enter();
	}
}

void gd_unlock_gdk(Desklet *me)
{
	if (me->is_multithreaded)
	{
		gdk_threads_leave();
	}
}

gboolean gd_draw_win_bg(Desklet *me)
{
	//Attaches background image portion for our window
	guint  width, height, border, depth, dum4, dum5, dum6, pmapdepth;
	int  x, y, dum2, dum3;
	Window win, dum1;
	Pixmap x11pm, x11bg;
	
	//Get our window
	if (! me->win) gd_setup_win(me);
	win = GDK_WINDOW_XID(me->win);
	
	//Get geometry of our window
	if (! XGetGeometry(me->disp, win, &dum1, &x, &y, &width, &height, &border,
			&depth))
	{
		g_warning("XGetGeometry failed.");
		return FALSE;
	}
	XTranslateCoordinates(me->disp, win, me->rootwin, 0, 0, &x, &y, &dum1);
	
	//v2.1: Real transparency, if supported
	//You can comment out this block to disable it.
	if ( (gdk_screen_is_composited(gdk_screen_get_default()))
			&& (depth == 32) )
	{
		me->bgpix = me->trans_bg;
		
		gtk_rc_reparse_all_for_settings(gtk_settings_get_default(), FALSE);
		gdk_window_set_back_pixmap(me->win, me->bgpix, FALSE);
		return TRUE;
	}
	
	//Get background image into x11bg
	x11bg = x_get_root_pixmap_by_property(me->disp);
	if (x11bg == None)
	{
		g_warning("x_get_root_pixmap_by_property failed\n");
		return FALSE;
	}
	
	//Get depth of our wallpaper into pmapdepth
	if (! XGetGeometry(me->disp, x11bg, &dum1, &dum2, &dum3, &dum4, &dum5, &dum6,
			&pmapdepth))
	{
		g_warning("XGetGeometry failed.");
		return FALSE;
	}
	
	//Why to waste resources
	if (me->pseudo_bg) g_object_unref(G_OBJECT(me->pseudo_bg));
	
	//Are depths different?
	if (depth != pmapdepth)
	{
		//Then use GdkPixbuf to copy between different depths (bad luck)
		GdkPixmap *gdkbg = gdk_pixmap_foreign_new(x11bg);
		GdkPixbuf *clspix;
		int scrwidth = XWidthOfScreen(me->scr);
		int scrheight = XHeightOfScreen(me->scr);
		
		clspix = gdk_pixbuf_get_from_drawable(NULL,
				GDK_DRAWABLE(gdkbg), gdk_colormap_get_system(), x, y, 0, 0,
				MIN(width, (scrwidth - x) - 1),
				MIN(height, (scrheight - y) - 1));
		g_object_unref(G_OBJECT(gdkbg));
		me->pseudo_bg = gdk_pixmap_new(NULL, width, height, depth);
		if (! me->pseudo_bg)
		{
			g_warning("gdk_pixmap_new failed");
			return FALSE;
		}
		gdk_drawable_set_colormap(GDK_DRAWABLE(me->pseudo_bg),
				gdk_colormap_new(gdk_visual_get_best_with_depth(depth), FALSE));
		gdk_draw_pixbuf(GDK_DRAWABLE(me->pseudo_bg), NULL, clspix, 0, 0, 0, 0, -1, -1,
				GDK_RGB_DITHER_NONE, 0, 0);
		g_object_unref(G_OBJECT(clspix));
	}
	else
	{
		//Else directly copy the background
		Pixmap x11pm;
		GC gc;
		XGCValues  gcv;
		
		//Create new pixmap
		me->pseudo_bg = gdk_pixmap_new(NULL, width, height, pmapdepth);
		if (! me->pseudo_bg)
		{
			g_warning("gdk_pixmap_new failed");
			return FALSE;
		}
		x11pm = gdk_x11_drawable_get_xid(GDK_DRAWABLE(me->pseudo_bg));
		gdk_drawable_set_colormap(GDK_DRAWABLE(me->pseudo_bg),
				gdk_colormap_new(gdk_visual_get_best_with_depth(pmapdepth),
				FALSE));
				
		//We need a GC ... Get it from GCList
		gc = gc_list_get_gc(&(me->bg_gc_list), x11bg, pmapdepth);
		
		//Update graphics context
		gcv.tile = x11bg;
		XChangeGC(me->disp, gc, GCTile, &gcv);
		
		//Copy background image portion
		XSetTSOrigin(me->disp, gc, -x, -y);
		XFillRectangle(me->disp, x11pm, gc, 0, 0, width, height);
	}
	me->bgpix = me->pseudo_bg;
	//The pixmap me->bgpix now contains req'd background pixmap.
	//Now attach the pixmap to our window!
	
	gdk_window_set_back_pixmap(me->win, me->bgpix, FALSE);
	gtk_widget_queue_draw(me->wid);
	
	return TRUE;
}

void gd_fix_win_bg(Desklet *me)
{
	if (me->bgpix)
	{
		gdk_window_set_back_pixmap(me->win, me->bgpix, FALSE);
	}
	else
		gd_draw_win_bg(me);
	
}

void gd_event_filter_func(GdkEvent *event, gpointer data)
{
	//Using this to events
	Desklet *me = (Desklet *) data;
	char *propname;
	
	if ( (event->type == GDK_CONFIGURE) && check_window_moved(me->win) )
	{
		if (me->transparent) gd_draw_win_bg(me);
	}
	else if ( (event->type == GDK_PROPERTY_NOTIFY) )
	{
		propname = gdk_atom_name(event->property.atom);
		if ((strcmp(propname, "_XROOTPMAP_ID") == 0)
				|| (strcmp(propname, "ESETROOT_PMAP_ID") == 0) )
		{
			if (me->transparent) gd_draw_win_bg(me);
		}
	}
	
	gtk_main_do_event(event);
	
	if (event->type == GDK_CLIENT_EVENT)
	{
		if (me->transparent) gd_fix_win_bg(me);
	}
}

void gd_event_filter_setup_func(gpointer data)
{
	gdk_event_handler_set(gd_event_filter_func, data,
		gd_event_filter_setup_func);
}

void gd_cb_on_composited_changed(GdkScreen *object, gpointer data)
{
	Desklet *me = (Desklet *) data;
	if (me->transparent) gd_draw_win_bg(me);
}

//Window searching using GObject::constructed
void (*gd_old_func) (GObject *instance) = NULL;
static Desklet desk;

gboolean gd_trans_init(Desklet *me, GtkWidget *wid)
{
	//Initialises all transparency stuff
	XGCValues gcv;
	gulong mask;
	GC alpha_gc;
	Pixmap x11pm;
	static gboolean called = FALSE;
	
	me->win = wid->window;
	me->wid = wid;
	
	if (called) return FALSE;
	called = TRUE;
	
	//Restore the hacked stuff...
	GObjectClass *klass = g_type_class_peek(GTK_TYPE_WINDOW);
	if (gd_old_func) klass->constructed = gd_old_func;
	
	//Check whether we should do that
	if (me->disp == NULL)
	{
		g_warning("Couldn't open default display.");
		me->transparent = FALSE;
	}
	else if (g_getenv("DISABLE_TRANSPARENCY"))
		me->transparent = FALSE;
	else
	{
		me->transparent = TRUE;
		//Compositing change notification
		g_signal_connect(gdk_screen_get_default(), "composited-changed",
				G_CALLBACK(gd_cb_on_composited_changed), me);
		
	}
	
	if (! me->transparent) return;
	
	//Initialise variables
	me->bgpix = NULL;
	me->pseudo_bg = NULL;
	
	//Initialise GCList structure used for pseudotransparency
	me->bg_gc_list.gcv.ts_x_origin = 0;
	me->bg_gc_list.gcv.ts_y_origin = 0;
	me->bg_gc_list.gcv.fill_style = FillTiled;
	me->bg_gc_list.mask = GCTileStipXOrigin | GCTileStipYOrigin | GCFillStyle;
	me->bg_gc_list.list = NULL;
	me->bg_gc_list.disp = me->disp;
	
	//Create background pixmap used for real transparency
	gcv.background = 0;
	gcv.fill_style = FillSolid;
	mask = GCBackground | GCFillStyle;
	alpha_gc = XCreateGC(me->disp, GDK_WINDOW_XID(me->win), mask, &gcv);
	
	me->trans_bg = gdk_pixmap_new(GDK_DRAWABLE(me->win), 1, 1, -1);
	x11pm = gdk_x11_drawable_get_xid(GDK_DRAWABLE(me->trans_bg));
	XFillRectangle(me->disp, x11pm, alpha_gc, 0, 0, 1, 1);
	XFreeGC(me->disp, alpha_gc);
	
	//Transparency-related window properties
	gtk_widget_set_app_paintable(me->wid, TRUE);
	check_window_moved(me->win);
	gd_draw_win_bg(me);
			
	//Wallpaper switch notification
	gd_event_filter_setup_func(me);
	gdk_window_set_events(gdk_get_default_root_window(),
			GDK_PROPERTY_CHANGE_MASK);
	
}

//Messages
int msg_socks[2];
GIOChannel *sock_chnls[2];
#define GD_EVENT_REFRESH_GTKTHEME ("refresh\n")

void gd_sigusr1_handler(gint signum)
{
	//Write event to the socket pair (This function can be in middle of a
	//work
	g_io_channel_write_chars(sock_chnls[0], GD_EVENT_REFRESH_GTKTHEME,
			-1, NULL, NULL);
	g_io_channel_flush(sock_chnls[0], NULL);
}

gboolean gd_signal_rcvd_func(GIOChannel *chnl, GIOCondition cdn,
		gpointer me)
{
	char *buf = NULL;
	//Process the event here
	gd_lock_gdk((Desklet *) me);
	g_io_channel_read_line(chnl, &buf, NULL, NULL, NULL);
	if (buf)
	{
		gtk_rc_reparse_all_for_settings(gtk_settings_get_default(), TRUE);
		gd_fix_win_bg((Desklet *) me);
		g_free(buf);
	}
	gd_unlock_gdk((Desklet *) me);
	return TRUE;
}

gboolean gd_setup_win(Desklet *me, GObject *object)
{	
	if (! GTK_IS_WINDOW(object)) return;
	GtkWidget *win = GTK_WIDGET(object);
	
	//g_debug("Window found");
	
	//Window found, now changing properties
	//gtk_widget_unmap(win);
	gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
	if (strcmp(gdk_x11_screen_get_window_manager_name(
			gdk_screen_get_default()), "JWM") == 0)
	{
		gtk_window_set_type_hint(GTK_WINDOW(win),
				GDK_WINDOW_TYPE_HINT_DESKTOP);
	}
	else
		gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_DOCK);
	
	gtk_window_set_keep_below(GTK_WINDOW(win), TRUE);
	gtk_window_stick(GTK_WINDOW(win));
	
	g_signal_connect_swapped(win, "show", G_CALLBACK(gd_trans_init), me);
	
	//gtk_widget_show(win);
	
	//GTK theme refreshing...
	if (socketpair(PF_LOCAL, SOCK_STREAM, 0, msg_socks) == 0)
	{
		int i;
		signal(SIGUSR1, gd_sigusr1_handler);
		for (i = 0; i < 2; i++)
		{
			gint flags;
			sock_chnls[i] = g_io_channel_unix_new(msg_socks[i]);
			flags = g_io_channel_get_flags(sock_chnls[i]);
			flags |= G_IO_FLAG_NONBLOCK;
			g_io_channel_set_flags(sock_chnls[i], flags, NULL);
		}
		g_io_add_watch(sock_chnls[1], G_IO_IN, gd_signal_rcvd_func, me);
	}
	else
		g_critical("unable to setup message system");
	
	return TRUE;
}

void gd_constructed(GObject *instance)
{
	if (gd_old_func) gd_old_func(instance);

	gd_setup_win(&desk, instance);
}

gboolean gd_init(Desklet *me)
{
	//Initialise everything
	int i;
	GdkScreen *screen;
	char *pid_str;
	GObjectClass *klass;
	
	//Lock GDK
	gd_lock_gdk(me);
	
	//Open default display and screen
	me->disp = gdk_x11_get_default_xdisplay();
	screen = gdk_screen_get_default();
	me->scr = gdk_x11_screen_get_xscreen(screen);
	
	//Get root window
	me->rootwin = XDefaultRootWindow(me->disp);
	
	//v3.1: Crazy hack in GObject to detect our window ;)
	klass = G_OBJECT_CLASS(g_type_class_ref(GTK_TYPE_WINDOW));
	gd_old_func = klass->constructed;
	klass->constructed = gd_constructed;
	
	//Write necessary info to environment
	pid_str = g_strdup_printf("%d", getpid());
	g_setenv("GTK2_DESKLET_PID", pid_str, TRUE);
	g_free(pid_str);
	
	gd_unlock_gdk(me);
	return FALSE;
}
///////////////////////////////////////////////////////////////////////////////

static GdkColormap *colormap;

G_MODULE_EXPORT void gtk_module_init (gint * argc, gchar *** argv)
{
	char **gtkrc_list = NULL;
	char *rc_string;
	int i;
	
	//v2.3: Beware of multithreaded gtkdialog
	if (g_thread_supported()) desk.is_multithreaded = TRUE;
	
	//gtk_init_add((GtkFunction) gd_init, &desk);
	
	
	//If using compositing manager then use RGBA colormap for real transparency
	if ((! g_getenv("DISABLE_RGBA_TRANS"))
			&& gdk_screen_is_composited(gdk_screen_get_default()))
	{
		colormap = gdk_screen_get_rgba_colormap(gdk_screen_get_default());
		gtk_widget_push_colormap(colormap);                
		gtk_widget_set_default_colormap(colormap);
	}
	
	//v3.0: GTK's resource files
	rc_string = (char *) g_getenv("DESKLET_GTKRC_FILES");
	if (rc_string) gtkrc_list = g_strsplit(rc_string, "|", -1);
	if (gtkrc_list != NULL)
	{
		for (i = 0; gtkrc_list[i] != NULL; i++)
		{
			if (gtkrc_list[i][0] != '\0')
				gtk_rc_add_default_file(gtkrc_list[i]);
		}
	}
	
	//v3.1: Use GObject::constructed instead of searching for toplevels
	gd_init(&desk);
}

