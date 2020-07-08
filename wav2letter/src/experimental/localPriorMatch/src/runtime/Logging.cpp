/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "experimental/localPriorMatch/src/runtime/Logging.h"

#include <cereal/archives/json.hpp>
#include <cereal/types/unordered_map.hpp>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include "common/FlashlightUtils.h"
#include "experimental/localPriorMatch/src/runtime/Defines.h"
#include "runtime/Serial.h"

namespace w2l {

LogHelper::LogHelper(
    int runIdx,
    std::string runPath,
    bool isMaster,
    bool logOnEpoch)
    : runIdx_(runIdx),
      runPath_(runPath),
      isMaster_(isMaster),
      logOnEpoch_(logOnEpoch) {
  if (isMaster_) {
    logFileName_ = getRunFile("log", runIdx_, runPath_);
    perfFileName_ = getRunFile("perf", runIdx_, runPath_);
    dirCreate(runPath_);
    std::ofstream logFile, perfFile;
    logFile.open(logFileName_);
    if (!logFile.is_open()) {
      LOG(FATAL) << "failed to open log file for writing";
    }
    perfFile.open(perfFileName_);
    if (!perfFile.is_open()) {
      LOG(FATAL) << "failed to open perf file for writing";
    }
  }
}

void LogHelper::saveConfig(
    const std::unordered_map<std::string, std::string>& config) {
  if (!isMaster_) {
    return;
  }

  std::ofstream configFile(getRunFile("config", runIdx_, runPath_));
  cereal::JSONOutputArchive ar(configFile);
  ar(CEREAL_NVP(config));
}

void LogHelper::writeHeader(SSLTrainMeters& meters) {
  if (!isMaster_) {
    return;
  }

  std::ofstream perfFile;
  perfFile.open(perfFileName_);
  auto perfMsg = formatStatus(meters, 0, {}, false, true, "\t", true);
  appendToLog(perfFile, "# " + perfMsg);
}

void LogHelper::logStatus(
    SSLTrainMeters& mtrs,
    int64_t epoch,
    const std::unordered_map<std::string, double>& logFields) {
  syncMeter(mtrs);

  if (!isMaster_) {
    return;
  }

  try {
    std::ofstream logFile, perfFile;
    logFile.open(logFileName_, std::ofstream::out | std::ofstream::app);
    perfFile.open(perfFileName_, std::ofstream::out | std::ofstream::app);
    auto logMsg =
        formatStatus(mtrs, epoch, logFields, true, false, " | ", false);
    auto perfMsg =
        formatStatus(mtrs, epoch, logFields, false, true, " ", false);
    LOG_MASTER(INFO) << logMsg;
    appendToLog(logFile, logMsg);
    appendToLog(perfFile, perfMsg);
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Error while writing logs: " << ex.what();
  }
}

void LogHelper::saveModel(
    const std::string& tag,
    const std::unordered_map<std::string, std::string>& config,
    std::shared_ptr<fl::Module> network,
    std::shared_ptr<SequenceCriterion> criterion,
    std::shared_ptr<LMCritic> lmcrit,
    std::shared_ptr<fl::FirstOrderOptimizer> netoptim) {
  if (!isMaster_) {
    return;
  }

  try {
    std::string filename =
        getRunFile("model_" + cleanFilepath(tag) + ".bin", runIdx_, runPath_);
    W2lSerializer::save(filename, config, network, criterion, netoptim, lmcrit);
  } catch (const std::exception& ex) {
    LOG(FATAL) << "Error while saving models: " << ex.what();
  }
}

void LogHelper::saveProposalModel(
    const std::unordered_map<std::string, std::string>& config,
    std::shared_ptr<fl::Module> network,
    std::shared_ptr<SequenceCriterion> criterion) {
  if (!isMaster_) {
    return;
  }

  try {
    std::string filename = getRunFile("prop.bin", runIdx_, runPath_);
    W2lSerializer::save(filename, config, network, criterion);
  } catch (const std::exception& ex) {
    LOG(FATAL) << "Error while saving proposal models: " << ex.what();
  }
}

std::string LogHelper::saveWorkerProposalModel(
    const std::unordered_map<std::string, std::string>& config,
    std::shared_ptr<fl::Module> network,
    std::shared_ptr<SequenceCriterion> criterion,
    int worldRank) {
  std::string basename = format("prop_worker%03d.bin", worldRank);
  std::string path = getRunFile(basename, runIdx_, runPath_);
  try {
    W2lSerializer::save(path, config, network, criterion);
  } catch (const std::exception& ex) {
    LOG(FATAL) << "Error while saving proposal models: " << ex.what();
  }
  return path;
}

void LogHelper::logAndSaveModel(
    SSLTrainMeters& meters,
    const std::unordered_map<std::string, std::string>& config,
    std::shared_ptr<fl::Module> network,
    std::shared_ptr<SequenceCriterion> criterion,
    std::shared_ptr<LMCritic> lmcrit,
    std::shared_ptr<fl::FirstOrderOptimizer> netoptim,
    const std::unordered_map<std::string, double>& logFields) {
  int iter = logOnEpoch_ ? std::stoi(config.at(kEpoch))
                         : std::stoi(config.at(kIteration));
  std::string tag = "last";
  if (FLAGS_itersave) {
    tag = logOnEpoch_ ? format("epoch_%04d", iter) : format("iter_%08d", iter);
  }

  logStatus(meters, iter, logFields);
  saveModel(tag, config, network, criterion, lmcrit, netoptim);

  for (auto& s : meters.valid) {
    double verr = s.second.edits[kTarget].value()[0];
    auto sit = validminerrs_.find(s.first);
    if (sit == validminerrs_.end() || sit->second > verr) {
      validminerrs_[s.first] = verr;
      saveModel(s.first, config, network, criterion, lmcrit, netoptim);
    }
  }
}

std::string LogHelper::formatStatus(
    SSLTrainMeters& meters,
    int64_t epoch,
    const std::unordered_map<std::string, double>& logFields,
    bool verbose /* = false */,
    bool date /* = false */,
    const std::string& separator /* = " " */,
    bool headerOnly /* = false */) {
  std::string header, status;

  auto insertItem = [&](std::string key, std::string val) {
    if (verbose) {
      val = key + ": " + val;
    }
    header = header + (header.empty() ? "" : separator) + key;
    status = status + (status.empty() ? "" : separator) + val;
  };

  auto insertSSLDatasetMeters = [&insertItem](
                                    SSLDatasetMeters& meter, std::string tag) {
    for (auto& m : meter.losses) {
      insertItem(
          tag + "-loss-" + m.first, format("%10.5f", m.second.value()[0]));
    }
    for (auto& m : meter.edits) {
      insertItem(
          tag + "-" + m.first + "ER", format("%5.2f", m.second.value()[0]));
    }
  };

  if (date) {
    insertItem("date", format("%s", getCurrentDate().c_str()));
    insertItem("time", format("%s", getCurrentTime().c_str()));
  }

  if (logOnEpoch_) {
    insertItem("epoch", format("%8d", epoch));
  } else {
    insertItem("iter", format("%8d", epoch));
  }

  insertItem("lr", headerOnly ? "" : format("%4.6lf", logFields.at("lr")));
  insertItem(
      "lmcrit-t", headerOnly ? "" : format("%4.6lf", logFields.at("lmcrit-t")));

  int rt = meters.timer[kRuntime].value();
  insertItem(
      kRuntime,
      format("%02d:%02d:%02d", (rt / 60 / 60), (rt / 60) % 60, rt % 60));

  for (auto& m : meters.timer) {
    if (m.first == kRuntime) {
      continue;
    }
    insertItem(m.first + "(ms)", format("%.2f", m.second.value() * 1000));
  }

  insertSSLDatasetMeters(meters.train, "train");
  for (auto& v : meters.valid) {
    insertSSLDatasetMeters(v.second, v.first);
  }

  auto stats = meters.stats.value();
  auto numsamples = std::max<int64_t>(stats[4], 1);
  auto isztotal = stats[0];
  auto tsztotal = stats[1];
  auto tszmax = stats[3];
  insertItem("avg-isz", format("%03d", isztotal / numsamples));
  insertItem("avg-tsz", format("%03d", tsztotal / numsamples));
  insertItem("max-tsz", format("%03d", tszmax));

  double audioProcSec = isztotal * FLAGS_batchsize;
  if (FLAGS_pow || FLAGS_mfcc || FLAGS_mfsc) {
    audioProcSec = audioProcSec * FLAGS_framestridems / 1000.0;
  } else {
    audioProcSec /= FLAGS_samplerate;
  }
  auto worldSize = fl::getWorldSize();
  double timeTakenSec = meters.timer[kTimer].value() * numsamples / worldSize;

  insertItem("hrs", format("%7.2f", audioProcSec / 3600.0));
  insertItem(
      "thrpt(sec/sec)",
      timeTakenSec > 0.0 ? format("%.2f", audioProcSec / timeTakenSec) : "n/a");
  return headerOnly ? header : status;
}

template <>
void syncMeter<SSLTrainMeters>(SSLTrainMeters& meters) {
  syncMeter(meters.stats);
  for (auto& m : meters.timer) {
    syncMeter(m.second);
  }
  syncMeter(meters.train);
  for (auto& m : meters.valid) {
    syncMeter(m.second);
  }
}

template <>
void syncMeter<SSLDatasetMeters>(SSLDatasetMeters& meters) {
  for (auto& m : meters.edits) {
    syncMeter(m.second);
  }
  for (auto& m : meters.losses) {
    syncMeter(m.second);
  }
}

void resetTimeStatMeters(SSLTrainMeters& meters) {
  for (auto& m : meters.timer) {
    m.second.reset();
  }
  meters.stats.reset();
}

void stopTimeMeters(SSLTrainMeters& meters) {
  for (auto& m : meters.timer) {
    m.second.stop();
  }
}

void resetDatasetMeters(SSLDatasetMeters& meters) {
  for (auto& m : meters.edits) {
    m.second.reset();
  }
  for (auto& m : meters.losses) {
    m.second.reset();
  }
}

} // namespace w2l
