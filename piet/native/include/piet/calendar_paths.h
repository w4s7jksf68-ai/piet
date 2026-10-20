#ifndef PIET_NATIVE_CALENDAR_PATHS_H
#define PIET_NATIVE_CALENDAR_PATHS_H

namespace piet {
class SessionState;
namespace handlers {
// Resolve calendar directories from the same PERSONAL setting as sysdir.
void configureCalendarSearch(SessionState& session);
}
}

#endif
