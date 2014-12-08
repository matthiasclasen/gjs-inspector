/* -*- mode: js2; js2-basic-offset: 4; indent-tabs-mode: nil -*- */

var commandHeader =
  "const GLib = imports.gi.GLib;" +
  "const GObject = imports.gi.GObject;" +
  "const Gio = imports.gi.Gio;" +
  "const Pango = imports.gi.Pango;" +
  "const Cairo = imports.cairo;" +
  "const Gtk = imports.gi.Gtk;"

const JsParse = imports.inspector.jsParse;

function complete (text)
{
    let [completions, attrHead] = JsParse.getCompletions(text, commandHeader, null);
    if (completions.length == 0) {
        __inspector.completion_label.hide ();
        __inspector.completion_label.error_bell ();
    } else if (completions.length == 1) {
        __inspector.entry.emit("insert_at_cursor", completions[0].slice(attrHead.length));
        __inspector.completion_label.hide ();
    } else {
        let commonPrefix = JsParse.getCommonPrefix(completions);
        __inspector.completion_label.set_text(completions.join(", "))
        __inspector.completion_label.show ();
        __inspector.entry.emit("insert_at_cursor", commonPrefix.slice(attrHead.length));
    }
}

function eval_line (arg)
{
    __inspector.completion_label.hide ();
    return eval (commandHeader + arg);
}