/* PIET native - Windows resource identifiers.
 *
 * Included by resources/windows/piet.rc.in (via the configured copy that
 * CMake drops next to the generated piet.rc) and by nothing else today.
 *
 * ASCII ONLY. This header is fed to windres' C preprocessor together with
 * the .rc; keep it free of anything that is not a plain #define, and free of
 * non-ASCII bytes -- windres reads the script as raw bytes for the default
 * (non-Unicode) codepage, so a stray UTF-8 character in a comment is an
 * avoidable encoding hazard.
 *
 * Numbering: application icons live at 101+. Windows Explorer shows the
 * LOWEST-numbered RT_GROUP_ICON as the file's icon, so IDI_PIET must stay
 * the smallest icon id in the binary if further icons are ever added.
 */
#ifndef PIET_NATIVE_WINDOWS_RESOURCE_H
#define PIET_NATIVE_WINDOWS_RESOURCE_H

#define IDI_PIET 101

#endif /* PIET_NATIVE_WINDOWS_RESOURCE_H */
