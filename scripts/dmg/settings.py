# dmgbuild settings for the Mac disk image (scripts/make-dmg.sh passes -D app=<path> and
# -D background=<background.tiff>). The window shows the background (render.sh draws it from
# dmg.html) with the app on the left and a link to Applications on the right; the icon
# centres match dmg.html.
import os.path

app = defines["app"]  # noqa: F821 (dmgbuild provides `defines`)
name = os.path.basename(app)

format = "ULFO"          # lzfse: read by every macOS this app runs on, and packs well
filesystem = "APFS"
files = [app]
symlinks = {"Applications": "/Applications"}
icon = os.path.join(app, "Contents", "Resources", "AppIcon.icns")  # the volume's own icon

background = defines["background"]  # noqa: F821
window_rect = ((200, 120), (720, 440 + 32))  # Finder counts the title bar in the height
default_view = "icon-view"
show_toolbar = False
show_status_bar = False
show_pathbar = False
show_sidebar = False
show_tab_view = False
icon_size = 128
text_size = 13
icon_locations = {name: (283, 252), "Applications": (473, 252)}
hide_extensions = [name]  # "PvZ2 Reflourished", unless the Mac shows all extensions
