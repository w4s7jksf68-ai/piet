// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
namespace piet::dispatch_detail {
bool resolveProgramCommand(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& echo,
    ojson& blocks,
    std::string& cmdForDispatch,
    std::string& lineAsTyped) {


  // ------------------------------------------------------------------
  // 1c. U5 (S6): user-program invocation — BEFORE the data guard (a program
  // may itself `use`/`sysuse` before touching data). Uses the tokenized
  // parsed.cmd (ORIGINAL case) for case-insensitive program-name lookup,
  // mirroring dispatcher.py's `_find_program(session, parsed.cmd)`.
  {
    // Inside an ado file's program, that file's private programs come first.
    std::optional<std::string> progMatch;
    if (!session.adoContext.empty())
      progMatch = program::findProgram(session, session.adoContext + "." + parsed.cmd);
    if (!progMatch.has_value()) progMatch = program::findProgram(session, parsed.cmd);
    // BUG-442/BUG-1445: a name that is neither a program nor a PIET command is
    // looked up as `<name>.ado` on the adopath (the current directory is on
    // it by default, and `adopath +` extends it), exactly as Stata resolves
    // an unknown command. The file is run once, silently; the program it
    // defines under the command's own name is then called, and its other
    // programs stay private to it. A file that does not define the command is
    // Stata's r(199) "command NAME not defined by NAME.ado".
    if (!progMatch.has_value() && registry::findByName(lower(parsed.cmd)) == nullptr &&
        std::regex_match(parsed.cmd, std::regex("[A-Za-z_][A-Za-z0-9_]{0,31}"))) {
      if (std::optional<std::string> adoFile =
              handlers::findOnAdopath(session, parsed.cmd + ".ado")) {
        // The programs as they were: a file's program that shares its name with
        // one already defined (a helper `v1help` beside the user's own) is the
        // file's private one, and the user's stays as it was (StataBE `program
        // dir` lists `v1cmd.v1help` and `v1help` apart).
        const auto before = session.programs;
        doexec::ExecuteFileResult fres;
        {
          doexec::PrivateLocalScope privateScope(session);
          // A `version` at the file's top level is the file's own (StataBE: the
          // caller's 16 holds inside the command and after it).
          CallerVersionScope versionScope(session);
          fres = doexec::executeDo(*adoFile, session, dispatch, {});
        }
        std::vector<std::string> defined;
        for (const auto& kv : session.programs) {
          auto old = before.find(kv.first);
          if (old == before.end() || old->second.body != kv.second.body ||
              old->second.rclass != kv.second.rclass)
            defined.push_back(kv.first);
        }
        for (const auto& name : defined) {
          ProgramDef def = std::move(session.programs.at(name));
          session.programs.erase(name);
          if (auto old = before.find(name); old != before.end()) session.programs[name] = old->second;
          def.ado = parsed.cmd;
          if (lower(name) == lower(parsed.cmd)) {
            session.programs[name] = std::move(def);
          } else {
            const std::string privateName = parsed.cmd + "." + name;
            def.name = privateName;
            session.programs[privateName] = std::move(def);
          }
        }
        if (fres.result.stoppedAtReason.has_value()) {
          ojson out = ojson::array({echo});
          for (const auto& b : fres.result.blocks)
            if (b.value("type", std::string()) == "error") out.push_back(b);
          { blocks = out; return true; }
        }
        progMatch = program::findProgram(session, parsed.cmd);
        if (!progMatch.has_value()) {
          ojson err = ojson(eval::errorBlock("unsupported_command",
                                             "command " + parsed.cmd + " not defined by " +
                                                 parsed.cmd + ".ado",
                                             parsed.cmd));
          err["reason"] = "r(199)";
          { blocks = ojson::array({echo, err}); return true; }
        }
      }
    }
    if (progMatch.has_value()) {
      const ProgramDef& progDef = session.programs.at(*progMatch);
      struct AdoContextScope {
        SessionState& s;
        std::string saved;
        AdoContextScope(SessionState& ss, const std::string& ado) : s(ss), saved(ss.adoContext) {
          s.adoContext = ado;
        }
        ~AdoContextScope() { s.adoContext = saved; }
      } adoScope(session, progDef.ado);
      CallerVersionScope versionScope(session);  // BUG-1459: the program's own `version`
      // Argument string: everything after the program-name token in the
      // macro-expanded line (cmdForDispatch), mirroring
      // raw_cmd_for_dispatch[find(parsed.cmd)+len:].strip().
      // BUG-1049: from the line as typed (lineAsTyped), so `0' keeps its `"..."'.
      const std::string& cmdAsTyped = lineAsTyped.empty() ? cmdForDispatch : lineAsTyped;
      std::string afterCmd;
      size_t pos = cmdAsTyped.find(parsed.cmd);
      if (pos != std::string::npos) {
        afterCmd = trimWs(cmdAsTyped.substr(pos + parsed.cmd.size()));
      }
      ojson progBlocks;
      try {
        progBlocks = program::runProgram(progDef, afterCmd, session, dispatch);
      } catch (const std::exception& exc) {
        { blocks = ojson::array({echo, eval::engineError("program " + parsed.cmd + ": " + exc.what())}); return true; }
      }
      // A noisily inside the program does not undo a quietly around its call
      // (StataBE 19.5: `quietly pn` prints nothing of pn's `noisily display`).
      loops::settleNoisily(progBlocks, session, /*force=*/true);
      ojson out = ojson::array({echo});
      for (const auto& b : progBlocks) out.push_back(b);
      { blocks = out; return true; }
    }
  }
  return false;
}

}
