// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Registered option sets, shared by the command modules that consume them.
#pragma once
#include "piet/eval/errors.h"

namespace piet::handlers::estimation_vocab {
extern const eval::OptVocab& kVocabTtest;
extern const eval::OptVocab& kVocabCsdid;
extern const eval::OptVocab& kVocabReg3;
extern const eval::OptVocab& kVocabBoottest;
extern const eval::OptVocab& kVocabTobit;
extern const eval::OptVocab& kVocabHeckman;
extern const eval::OptVocab& kVocabQreg;
extern const eval::OptVocab& kVocabMediate;
extern const eval::OptVocab& kVocabMediateTreat;
extern const eval::OptVocab& kVocabDrdid;
extern const eval::OptVocab& kVocabMargins;
extern const eval::OptVocab& kVocabEstatDurbinalt;
extern const eval::OptVocab& kVocabEstatEvent;
extern const eval::OptVocab& kVocabNewey;
extern const eval::OptVocab& kVocabPrais;
extern const eval::OptVocab& kVocabWntestq;
extern const eval::OptVocab& kVocabSdtest;
extern const eval::OptVocab& kVocabHausman;
extern const eval::OptVocab& kVocabChow;
extern const eval::OptVocab& kVocabMkspline;
extern const eval::OptVocab& kVocabIvRegress;
extern const eval::OptVocab& kVocabPoisson;
extern const eval::OptVocab& kVocabNbreg;
extern const eval::OptVocab& kVocabXtPoissonLogit;
extern const eval::OptVocab& kVocabDfullerPperron;
extern const eval::OptVocab& kVocabProbitRobust;
}

namespace piet::handlers::extended_estimation_vocab {
extern const std::unordered_set<std::string> kXtPoissonLogitBase;
extern const eval::OptVocab& kVocabTtest;
extern const eval::OptVocab& kVocabCsdid;
extern const eval::OptVocab& kVocabReg3;
extern const eval::OptVocab& kVocabSureg;
extern const eval::OptVocab& kVocabBoottest;
extern const eval::OptVocab& kVocabTobit;
extern const eval::OptVocab& kVocabHeckman;
extern const eval::OptVocab& kVocabQreg;
extern const eval::OptVocab& kVocabMediate;
extern const eval::OptVocab& kVocabMediateTreat;
extern const eval::OptVocab& kVocabDrdid;
extern const eval::OptVocab& kVocabMargins;
extern const eval::OptVocab& kVocabEstatDurbinalt;
extern const eval::OptVocab& kVocabEstatEvent;
extern const eval::OptVocab& kVocabNewey;
extern const eval::OptVocab& kVocabPrais;
extern const eval::OptVocab& kVocabWntestq;
extern const eval::OptVocab& kVocabSdtest;
extern const eval::OptVocab& kVocabHausman;
extern const eval::OptVocab& kVocabChow;
extern const eval::OptVocab& kVocabMkspline;
extern const eval::OptVocab& kVocabIvRegress;
extern const eval::OptVocab& kVocabIvFirst;
extern const eval::OptVocab& kVocabPpmlD;
extern const eval::OptVocab& kVocabPoisson;
extern const eval::OptVocab& kVocabNbreg;
extern const eval::OptVocab& kVocabXtPoissonLogit;
extern const eval::OptVocab& kVocabDfullerPperron;
}

namespace piet::handlers::postestimation_vocab {
extern const std::unordered_set<std::string> kDfullerPperronBase;
extern const eval::OptVocab& kVocabTtest;
extern const eval::OptVocab& kVocabCsdid;
extern const eval::OptVocab& kVocabReg3;
extern const eval::OptVocab& kVocabBoottest;
extern const eval::OptVocab& kVocabTobit;
extern const eval::OptVocab& kVocabHeckman;
extern const eval::OptVocab& kVocabQreg;
extern const eval::OptVocab& kVocabMediate;
extern const eval::OptVocab& kVocabMediateTreat;
extern const eval::OptVocab& kVocabDrdid;
extern const eval::OptVocab& kVocabMargins;
extern const eval::OptVocab& kVocabEstatDurbinalt;
extern const eval::OptVocab& kVocabEstatEvent;
extern const eval::OptVocab& kVocabNewey;
extern const eval::OptVocab& kVocabPrais;
extern const eval::OptVocab& kVocabWntestq;
extern const eval::OptVocab& kVocabSdtest;
extern const eval::OptVocab& kVocabHausman;
extern const eval::OptVocab& kVocabChow;
extern const eval::OptVocab& kVocabMkspline;
extern const eval::OptVocab& kVocabIvRegress;
extern const eval::OptVocab& kVocabPoisson;
extern const eval::OptVocab& kVocabNbreg;
extern const eval::OptVocab& kVocabXtPoissonLogit;
extern const eval::OptVocab& kVocabDfullerPperron;
}

namespace piet::handlers::session_vocab {
extern const eval::OptVocab& kVocabLog;
extern const eval::OptVocab& kVocabMkdir;
}
