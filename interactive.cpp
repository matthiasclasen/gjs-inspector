/*
 * Copyright (c) 2014 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <glib/gi18n-lib.h>

#include <gjs/gjs.h>
#include <gjs/gjs-module.h>
#include <gi/object.h>

#include "interactive.h"

extern "C"
{
#include "resources.h"
}

struct _GtkInspectorInteractivePrivate
{
  gboolean in_init;
  GtkScrolledWindow *scrolled_window;
  GtkTextView *textview;
  GtkEntry *entry;
  GtkLabel *label;
  GtkLabel *completion_label;
  GjsContext *context;

  GObject *object;

  GString *buffer;

  GQueue            *history;
  GList             *history_current;
  gchar             *saved_text;
  int                saved_position;
  gboolean           saved_position_valid;
};

enum {
  PROP_0,
  PROP_OBJECT,
  PROP_COMPLETION_LABEL,
  PROP_ENTRY,
  PROP_TITLE,
  PROP_USE_PICKER,
  LAST_PROP
};

static GParamSpec *param_specs [LAST_PROP];

G_DEFINE_DYNAMIC_TYPE_EXTENDED (GtkInspectorInteractive,
                                gtk_inspector_interactive,
                                GTK_TYPE_BOX,
                                0,
                                G_ADD_PRIVATE_DYNAMIC(GtkInspectorInteractive))

static void error_reporter(JSContext *cx, const char *message, JSErrorReport *report);
static JSBool gtk_inspector_interactive_print (JSContext *context,
                                               unsigned   argc,
                                               jsval     *vp);

#define HISTORY_LENGTH 30

enum {
  COMPLETE,
  MOVE_HISTORY,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

/* This lists a bunch of imports in order to initialize these, as they seem
   to show a bunch of warning during initialization which we want to avoid */
static const char *init_js_code =
  "window.__complete = imports.inspector.repl.complete;\n"
  "window.__eval = imports.inspector.repl.evalLine;\n"
  "window.__objectChanged = imports.inspector.repl.objectChanged;\n"
  "imports.gi.Gio;\n"
  "imports.gi.Pango;\n"
  "imports.cairo;\n"
  "imports.gi.Gtk;\n";

static JSFunctionSpec global_funcs[] = {
    { "print", JSOP_WRAPPER (gtk_inspector_interactive_print), 0, GJS_MODULE_PROP_FLAGS },
    { NULL },
};

static void
gtk_inspector_interactive_init (GtkInspectorInteractive *interactive)
{
  JSContext *context;
  JSObject *global;
  const char *search_path[] = { "resource:///org/gnome/gjs-inspector/js", NULL };
  GjsContext *old_current;

  interactive->priv = (GtkInspectorInteractivePrivate*)gtk_inspector_interactive_get_instance_private (interactive);
  gtk_widget_init_template (GTK_WIDGET (interactive));
  interactive->priv->history = g_queue_new ();

  interactive->priv->buffer = g_string_new ("");

  gtk_label_set_lines (interactive->priv->completion_label, 7);

  old_current = gjs_context_get_current ();
  if (old_current)
    gjs_context_make_current (NULL);

  interactive->priv->context = (GjsContext *)g_object_new (GJS_TYPE_CONTEXT,
                                                           "search-path", search_path,
                                                           NULL);
  g_object_set_data (G_OBJECT (interactive->priv->context), "interactive", interactive);
  JS_SetErrorReporter ((JSContext *)gjs_context_get_native_context (interactive->priv->context), error_reporter);

  context = (JSContext *)gjs_context_get_native_context (interactive->priv->context);
  global = gjs_get_global_object (context);

  JSAutoCompartment ac(context, global);
  JSAutoRequest ar(context);
  jsval inspector;

  interactive->priv->in_init = TRUE;

  if (!JS_DefineFunctions(context, global, &global_funcs[0]))
    g_error("Failed to define properties on the global object");

  inspector.setObject(*gjs_object_from_g_object (context, G_OBJECT (interactive)));

  gjs_context_eval (interactive->priv->context,
                    init_js_code, -1, "<init>",
                    NULL, NULL);

  if (!JS_SetProperty(context, global, "__inspector", &inspector))
    g_error("Failed to define properties on the global object");

  interactive->priv->in_init = FALSE;

  if (old_current != NULL)
    {
      gjs_context_make_current (NULL);
      gjs_context_make_current (old_current);
    }
}

static void
gtk_inspector_interactive_finalize (GObject *object)
{
  GtkInspectorInteractive *interactive = GTK_INSPECTOR_INTERACTIVE (object);

  g_clear_object (&interactive->priv->object);
  g_clear_object (&interactive->priv->context);
  g_clear_pointer (&interactive->priv->saved_text, g_free);
  g_queue_free_full (interactive->priv->history, g_free);

  G_OBJECT_CLASS (gtk_inspector_interactive_parent_class)->finalize (object);
}


static void
gtk_inspector_interactive_constructed (GObject *object)
{
  G_OBJECT_CLASS (gtk_inspector_interactive_parent_class)->constructed (object);
}

static void
gtk_inspector_interactive_add_line (GtkInspectorInteractive *interactive,
                                    const char *str)
{
  GtkTextBuffer *buffer;
  GtkTextIter iter;
  GtkTextMark *insert_mark;

  buffer = gtk_text_view_get_buffer (interactive->priv->textview);
  gtk_text_buffer_get_end_iter (buffer, &iter);
  gtk_text_buffer_insert (buffer, &iter, str, -1);
  gtk_text_buffer_get_end_iter (buffer, &iter);
  gtk_text_buffer_insert (buffer, &iter, "\n", -1);

  insert_mark = gtk_text_buffer_get_insert (buffer);

  gtk_text_buffer_place_cursor (buffer, &iter);
  gtk_text_view_scroll_to_mark( interactive->priv->textview,
                                insert_mark, 0.0, TRUE, 0.0, 1.0);
}

static JSBool
gjs_print_parse_args (JSContext *context,
                      unsigned   argc,
                      jsval     *argv,
                      char     **buffer)
{
    GString *str;
    gchar *s;
    guint n;

    JS_BeginRequest (context);

    str = g_string_new ("");
    for (n = 0; n < argc; ++n)
      {
        JSExceptionState *exc_state;
        JSString *jstr;

        /* JS_ValueToString might throw, in which we will only
         * log that the value could be converted to string */
        exc_state = JS_SaveExceptionState(context);

        jstr = JS_ValueToString(context, argv[n]);
        if (jstr != NULL)
            argv[n] = STRING_TO_JSVAL(jstr); // GC root

        JS_RestoreExceptionState(context, exc_state);

        if (jstr != NULL)
          {
            if (!gjs_string_to_utf8(context, STRING_TO_JSVAL(jstr), &s))
              {
                JS_EndRequest(context);
                g_string_free(str, TRUE);
                return JS_FALSE;
              }

            g_string_append(str, s);
            g_free(s);
            if (n < (argc-1))
              g_string_append_c(str, ' ');
          }
        else
          {
            JS_EndRequest(context);
            *buffer = g_string_free(str, TRUE);
            if (!*buffer)
              *buffer = g_strdup("<invalid string>");
            return JS_TRUE;
          }
      }

    *buffer = g_string_free(str, FALSE);

    JS_EndRequest(context);
    return JS_TRUE;
}

static JSBool
gtk_inspector_interactive_print (JSContext *context,
                                 unsigned   argc,
                                 jsval     *vp)
{
  GjsContext *gjs_context = (GjsContext *) JS_GetContextPrivate (context);
  GtkInspectorInteractive *interactive;
  jsval *argv = JS_ARGV(context, vp);
  char *buffer;

  if (!gjs_print_parse_args(context, argc, argv, &buffer))
    return FALSE;

  interactive = GTK_INSPECTOR_INTERACTIVE (g_object_get_data (G_OBJECT (gjs_context), "interactive"));

  gtk_inspector_interactive_add_line (interactive, buffer);
  g_free (buffer);

  JS_SET_RVAL (context, vp, JSVAL_VOID);
  return JS_TRUE;
}

static void
error_reporter(JSContext *cx, const char *message, JSErrorReport *report)
{
  GjsContext *gjs_context = (GjsContext *) JS_GetContextPrivate (cx);
  GtkInspectorInteractive *interactive;
  GString *line;

  interactive = GTK_INSPECTOR_INTERACTIVE (g_object_get_data (G_OBJECT (gjs_context), "interactive"));

  if (interactive->priv->in_init)
    return;

  if (!report) {
    gtk_inspector_interactive_add_line (interactive, message);
    return;
  }

  /* Strict warnings in a repl are just a pain, lets ignore them */
  if (JSREPORT_IS_WARNING(report->flags) && JSREPORT_IS_STRICT(report->flags))
    return;

  line = g_string_new ("");
  if (report->filename)
    g_string_append_printf (line, "%s:", report->filename);
  if (report->lineno)
    g_string_append_printf (line, "%u:", report->lineno);
  if (JSREPORT_IS_WARNING(report->flags))
    g_string_append_printf (line, "%swarning: ",
                            JSREPORT_IS_STRICT(report->flags) ? "strict " : "");

  g_string_append_printf (line, message);

  gtk_inspector_interactive_add_line (interactive, line->str);

  g_string_free (line, TRUE);
}

static void
call (GtkInspectorInteractive *interactive,
      const char *function,
      const char *arg)
{
  JSContext *context;
  JSObject *global;
  jsval func, arg1, retval;
  char *str;
  GjsContext *old_current;


  context = (JSContext *)gjs_context_get_native_context (interactive->priv->context);
  global = gjs_get_global_object (context);

  old_current = gjs_context_get_current ();
  if (old_current != interactive->priv->context)
    {
      gjs_context_make_current (NULL);
      gjs_context_make_current (interactive->priv->context);
    }

  JSAutoCompartment ac(context, global);
  JSAutoRequest ar(context);

  arg1 = JSVAL_NULL;
  if (arg != NULL)
    {
      if (!gjs_string_from_utf8 (context, arg, -1, &arg1))
        g_error ("Failed to convert text to js");
    }

  if (!JS_GetProperty (context, global, function, &func))
    g_error ("No %s function to call", function);
  else if (!JS_CallFunctionValue (context, NULL, func, 1,
                                  &arg1, &retval))
    {
      if (JS_GetPendingException (context, &retval)) {
        str = gjs_value_debug_string (context, retval);
        if (str)
          {
            g_warning (str);
            g_free (str);
          }
      }
      JS_ClearPendingException(context);
    }

  if (old_current != interactive->priv->context)
    {
      gjs_context_make_current (NULL);
      gjs_context_make_current (old_current);
    }
}


static void
entry_activated (GtkEntry *entry,
                 GtkInspectorInteractive *interactive)
{
  JSContext *context;
  JSObject *global;
  const char *text;

  text = gtk_entry_get_text (entry);

  if (text[0] == 0)
    return;

  g_queue_push_head (interactive->priv->history, g_strdup (text));
  g_free (g_queue_pop_nth (interactive->priv->history, HISTORY_LENGTH));

  g_string_append (interactive->priv->buffer, text);

  context = (JSContext *)gjs_context_get_native_context (interactive->priv->context);
  global = gjs_get_global_object (context);

  JSAutoCompartment ac(context, global);
  JSAutoRequest ar(context);

  if (!JS_BufferIsCompilableUnit (context, NULL, interactive->priv->buffer->str, interactive->priv->buffer->len))
    {
      gtk_label_set_text (interactive->priv->label, "…");
    }
  else
    {
      call (interactive, "__eval", interactive->priv->buffer->str);

      g_string_set_size (interactive->priv->buffer, 0);
      gtk_label_set_text (interactive->priv->label, "» ");
    }

  interactive->priv->history_current = NULL;
  gtk_entry_set_text (entry, "");
}

static void
complete (GtkInspectorInteractive *interactive)
{
  const gchar *text;

  text = gtk_entry_get_text (interactive->priv->entry);

  call (interactive, "__complete", text);
}

static void
move_history (GtkInspectorInteractive *interactive,
              GtkDirectionType dir)
{
  GList *l;

  switch (dir)
    {
    case GTK_DIR_UP:
      l = interactive->priv->history_current;
      if (l == NULL)
        l = interactive->priv->history->head;
      else
        l = l->next;

      if (l == NULL)
        {
          gtk_widget_error_bell (GTK_WIDGET (interactive));
          return;
        }

      break;

    case GTK_DIR_DOWN:

      l = interactive->priv->history_current;
      if (l == NULL)
        {
          gtk_widget_error_bell (GTK_WIDGET (interactive));
          return;
        }

      l = l->prev;

      break;

    case GTK_DIR_TAB_FORWARD:
    case GTK_DIR_TAB_BACKWARD:
    case GTK_DIR_LEFT:
    case GTK_DIR_RIGHT:
    default:
      return;
    }

  if (interactive->priv->history_current == NULL)
    {
      g_clear_pointer (&interactive->priv->saved_text, g_free);
      interactive->priv->saved_text = g_strdup (gtk_entry_get_text (interactive->priv->entry));
    }
  interactive->priv->history_current = l;

  if (!interactive->priv->saved_position_valid)
    {
      interactive->priv->saved_position = gtk_editable_get_position (GTK_EDITABLE (interactive->priv->entry));
      if (interactive->priv->saved_position == gtk_entry_get_text_length (interactive->priv->entry))
        interactive->priv->saved_position = -1;
    }

  if (l == NULL)
    gtk_entry_set_text (interactive->priv->entry, interactive->priv->saved_text ? interactive->priv->saved_text : "");
  else
    gtk_entry_set_text (interactive->priv->entry, (char *)l->data);

  gtk_editable_set_position (GTK_EDITABLE (interactive->priv->entry), interactive->priv->saved_position);
  interactive->priv->saved_position_valid = TRUE;
}

static void
cursor_pos_changed (GtkEntry *entry,
                    GParamSpec	*pspec,
                    GtkInspectorInteractive *interactive)
{
  interactive->priv->saved_position_valid = FALSE;
}

static void
gtk_inspector_interactive_set_object (GtkInspectorInteractive *interactive,
                                      GObject *object)
{
  GObject *old;

  old = interactive->priv->object;

  interactive->priv->object = (GObject *)g_object_ref (object);
  if (old)
    g_object_unref (old);

  if (old != object)
    call (interactive, "__objectChanged", NULL);
}

static void
gtk_inspector_interactive_get_property (GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  GtkInspectorInteractive *interactive = GTK_INSPECTOR_INTERACTIVE (object);

  switch (prop_id)
    {
    case PROP_OBJECT:
      g_value_set_object (value, interactive->priv->object);
      break;

    case PROP_COMPLETION_LABEL:
      g_value_set_object (value, interactive->priv->completion_label);
      break;

    case PROP_ENTRY:
      g_value_set_object (value, interactive->priv->entry);
      break;

    case PROP_TITLE:
      g_value_set_string (value, "Interactive");
      break;

    case PROP_USE_PICKER:
      g_value_set_boolean (value, TRUE);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gtk_inspector_interactive_set_property (GObject      *object,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  GtkInspectorInteractive *interactive = GTK_INSPECTOR_INTERACTIVE (object);

  switch (prop_id)
    {
    case PROP_OBJECT:
      gtk_inspector_interactive_set_object (interactive, (GObject *)g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gtk_inspector_interactive_class_finalize (GtkInspectorInteractiveClass *klass)
{
}

static void
gtk_inspector_interactive_class_init (GtkInspectorInteractiveClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkBindingSet *binding_set;

  gtk_interactive_register_resource ();

  object_class->constructed = gtk_inspector_interactive_constructed;
  object_class->finalize = gtk_inspector_interactive_finalize;
  object_class->get_property = gtk_inspector_interactive_get_property;
  object_class->set_property = gtk_inspector_interactive_set_property;

  klass->move_history = move_history;
  klass->complete = complete;

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/gjs-inspector/interactive.ui");
  gtk_widget_class_bind_template_child_private (widget_class, GtkInspectorInteractive, entry);
  gtk_widget_class_bind_template_child_private (widget_class, GtkInspectorInteractive, label);
  gtk_widget_class_bind_template_child_private (widget_class, GtkInspectorInteractive, completion_label);
  gtk_widget_class_bind_template_child_private (widget_class, GtkInspectorInteractive, scrolled_window);
  gtk_widget_class_bind_template_child_private (widget_class, GtkInspectorInteractive, textview);

  gtk_widget_class_bind_template_callback (widget_class, entry_activated);
  gtk_widget_class_bind_template_callback (widget_class, cursor_pos_changed);

  param_specs [PROP_OBJECT] =
    g_param_spec_object ("object",
                         _("Object"),
                         _("This current object."),
                         G_TYPE_OBJECT,
                         (GParamFlags)(G_PARAM_READWRITE |
                                       G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_OBJECT,
                                   param_specs [PROP_OBJECT]);

  param_specs [PROP_COMPLETION_LABEL] =
    g_param_spec_object ("completion-label",
                         _("Completion label"),
                         _("Completion label."),
                         GTK_TYPE_LABEL,
                         (GParamFlags)(G_PARAM_READABLE |
                                       G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_COMPLETION_LABEL,
                                   param_specs [PROP_COMPLETION_LABEL]);

  param_specs [PROP_ENTRY] =
    g_param_spec_object ("entry",
                         _("entry"),
                         _("Entry."),
                         GTK_TYPE_ENTRY,
                         (GParamFlags)(G_PARAM_READABLE |
                                       G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_ENTRY,
                                   param_specs [PROP_ENTRY]);

  param_specs [PROP_TITLE] =
    g_param_spec_string ("title",
                         _("Title"),
                         _("Tab title."),
                         NULL,
                         (GParamFlags)(G_PARAM_READABLE |
                                       G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_TITLE,
                                   param_specs [PROP_TITLE]);

  param_specs [PROP_USE_PICKER] =
    g_param_spec_boolean ("use-picker",
                          _("Use Picker"),
                          _("Use Picker"),
                          TRUE,
                          (GParamFlags)(G_PARAM_READABLE |
                                        G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_USE_PICKER,
                                   param_specs [PROP_USE_PICKER]);


  signals[COMPLETE] =
    g_signal_new ("complete",
                  G_TYPE_FROM_CLASS (klass),
                  (GSignalFlags) (G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION),
                  G_STRUCT_OFFSET (GtkInspectorInteractiveClass, complete),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE,
                  0);

  signals[MOVE_HISTORY] =
    g_signal_new ("move-history",
                  G_TYPE_FROM_CLASS (klass),
                  (GSignalFlags) (G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION),
                  G_STRUCT_OFFSET (GtkInspectorInteractiveClass, move_history),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__ENUM,
                  G_TYPE_NONE,
                  1,
                  GTK_TYPE_DIRECTION_TYPE);

  binding_set = gtk_binding_set_by_class (klass);

  gtk_binding_entry_add_signal (binding_set,
                                GDK_KEY_Tab, (GdkModifierType)0,
                                "complete", 0);

  gtk_binding_entry_add_signal (binding_set,
                                GDK_KEY_Up, (GdkModifierType)0,
                                "move-history", 1,
                                GTK_TYPE_DIRECTION_TYPE, GTK_DIR_UP);
  gtk_binding_entry_add_signal (binding_set,
                                GDK_KEY_Down, (GdkModifierType)0,
                                "move-history", 1,
                                GTK_TYPE_DIRECTION_TYPE, GTK_DIR_DOWN);

}

void
gtk_inspector_interactive_grab_focus (GtkInspectorInteractive *interactive)
{
  gtk_widget_grab_focus (GTK_WIDGET (interactive->priv->entry));
}

void
gtk_inspector_interactive_register (GTypeModule *module)
{
  gtk_inspector_interactive_register_type (module);
  g_io_extension_point_implement ("gtk-inspector-page",
                                  GTK_TYPE_INSPECTOR_INTERACTIVE,
                                  "interactive",
                                  10);
}

// vim: set et sw=2 ts=2:
