/* --------------------------------------------------------------------------------
 * Author: Marcel Paz Goldschen-Ohm
 * Email: marcel.goldschen@gmail.com
 * -------------------------------------------------------------------------------- */

#include "StimulusClampProtocol.h"
#include "EigenLab.h"
#include "QObjectPropertyEditor.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <QApplication>
#include <QErrorMessage>
#include <QFile>
#include <QFileDialog>
#include <QFuture>
#include <QJsonDocument>
#include <QMessageBox>
#include <QTextStream>
#include <QTimer>
#include <QVariantMap>
#include <QtConcurrentRun>

namespace StimulusClampProtocol
{
    Eigen::RowVectorXd equilibriumProbability(const Eigen::MatrixXd &Q)
    {
        int N = Q.cols(); // # of states.
        // S is a copy of Q with one additional column of ones.
        Eigen::MatrixXd S = Eigen::MatrixXd::Ones(N, N + 1);
        S.block(0, 0, N, N) = Q;
        // u is a row vector of ones.
        Eigen::RowVectorXd u = Eigen::RowVectorXd::Ones(N);
        // Return u * ( S * S^T )^-1
        Eigen::MatrixXd St = S.transpose();
        return u * ((S * St).inverse());
    }
    
    void spectralExpansion(const Eigen::SparseMatrix<double> &Q, Eigen::VectorXd &eigenValues, std::vector<Eigen::MatrixXd> &spectralMatrices, AbortFlag *abort)
    {
        int N = Q.cols(); // # of states.
        if(N < 2) throw std::runtime_error("Spectral expansion for less than two states does not make sense.");
        Eigen::EigenSolver<Eigen::MatrixXd> eigenSolver(Q.toDense(), true);
        if(abort && *abort) return;
        Eigen::VectorXd eigVals = eigenSolver.pseudoEigenvalueMatrix().diagonal();
        // Get indexes of eigVals sorted in ascending order of their absolute value.
        std::vector<size_t> indexes(eigVals.size());
        std::iota(indexes.begin(), indexes.end(), 0); // indexes = {0, 1, 2, ...}
        // Sort indexes based on comparing values in eigenvalues.
        std::sort(indexes.begin(), indexes.end(), [&eigVals](size_t i1, size_t i2) { return fabs(eigVals[i1]) < fabs(eigVals[i2]); });
        if(abort && *abort) return;
        Eigen::MatrixXd invEigVecs = eigenSolver.pseudoEigenvectors().inverse();
        eigenValues = Eigen::VectorXd::Zero(N);
        spectralMatrices.assign(N, Eigen::MatrixXd::Zero(N, N));
        for(int i = 0; i < N; ++i) {
            if(abort && *abort) return;
            int j = indexes[i];
            eigenValues[i] = eigVals[j];
            spectralMatrices[i] = eigenSolver.pseudoEigenvectors().col(j) * invEigVecs.row(j);
        }
    }
    
    void findIndexesInRange(const Eigen::VectorXd &time, double start, double stop, int *firstPt, int *numPts, double epsilon)
    {
        *firstPt = -1;
        *numPts = 0;
        if(epsilon == 0)
            epsilon = std::numeric_limits<double>::epsilon() * 5;
        Eigen::VectorXd::Index closestIndex;
        (time.array() - start).abs().minCoeff(&closestIndex);
        *firstPt = closestIndex;
        if(time[*firstPt] < start - epsilon)
            ++(*firstPt);
        if(*firstPt < time.size()) {
            (time.array() - stop).abs().minCoeff(&closestIndex);
            int endPt = closestIndex;
            if(time[endPt] < stop - epsilon)
                ++endPt;
            *numPts = endPt - *firstPt;
        }
    }
    
    void sampleArray(double *xref, double *yref, int nref, double *x, double *y, int n, int *firstPt, int *numPts, double x0, double epsilon)
    {
        // Set y values in y(x) based on yref(xref - x0).
        // y(i) is lineraly interpolated between bounding yref(iref) and yref(iref + 1).
        // firstPt and numPts designate the indices of y(x) that have associated reference data in yref(xref - x0).
        // For example, if x = [10:20) and xref - x0 = [0:15), then firstPt = 0 and numPts = 5.
        // For example, if x = [10:20) and xref - x0 = [15:25), then firstPt = 5 and numPts = 5.
        // !!! WARNING !!! x and xref MUST be densely packed arrays that either increase or decrease monotonically.
        // !!! However, x can increase whereas xref decreases, or vice versa. The decreasing arrays are simply iterated in reverse.
        *firstPt = -1;
        *numPts = 0;
        if(epsilon == 0)
            epsilon = std::numeric_limits<double>::epsilon() * 5;
        bool isIncreasing = true;
        bool isRefIncreasing = true;
        if(n >= 2 && x[1] - x[0] < 0)
            isIncreasing = false;
        if(nref >= 2 && xref[1] - xref[0] < 0)
            isRefIncreasing = false;
        int i = (isIncreasing ? 0 : n - 1);
        int iref = (isRefIncreasing ? 0 : nref - 1);
        int di = (isIncreasing ? 1 : -1);
        int diref = (isRefIncreasing ? 1 : -1);
        while(i >= 0 && i < n && iref >= 0 && iref < nref) {
            if(x[i] < xref[iref] - x0 - epsilon) {
                // Ignore sample points before start of reference data.
                //y[i] = 0;
                i += di;
            } else if(fabs(x[i] - (xref[iref] - x0)) < epsilon) {
                y[i] = yref[iref];
                if(*firstPt == -1)
                    *firstPt = i;
                i += di;
                iref += diref;
            } else { // x[i] > xref[iref] - x0 + epsilon
                int jref = iref + diref;
                if(jref >= 0 && jref < nref && (xref[jref] - x0) > x[i]) {
                    // Interpolate (linear) our current sample point using the two encompassing reference data points.
                    // Then go to the next sample point.
                    double dx = xref[jref] - xref[iref];
                    double dy = yref[jref] - yref[iref];
                    y[i] = yref[iref] + (dy / dx) * (x[i] - (xref[iref] - x0));
                    if(*firstPt == -1)
                        *firstPt = i;
                    i += di;
                }
                iref = jref;
            }
        }
        if(*firstPt != -1) {
            if(isIncreasing) {
                *numPts = i - *firstPt;
            } else {
                *numPts = *firstPt - i;
                *firstPt = i + 1;
            }
        }
//        double dx, dy;
//        double *xref0 = const_cast<double*>(xref);
//        bool isIncreasing = true;
//        bool isRefIncreasing = true;
//        if(n >= 2 && x[1] - x[0] < 0)
//            isIncreasing = false;
//        if(nref >= 2 && xref[1] - xref[0] < 0)
//            isRefIncreasing = false;
//        int i = (isIncreasing ? 0 : n - 1);
//        int iref = (isRefIncreasing ? 0 : nref - 1);
//        int di = (isIncreasing ? 1 : -1);
//        int diref = (isRefIncreasing ? 1 : -1);
//        bool hasShiftedData = false;
//        if(x0 != 0) {
//            xref0 = new double[nref];
//            hasShiftedData = true;
//            xref0[iref] = xref[iref] - x0;
//        }
//        while(i >= 0 && i < n && iref + diref >= 0 && iref + diref < nref) {
//            if(hasShiftedData)
//                xref0[iref + diref] = xref[iref + diref] - x0;
//            if(x[i] < xref0[iref] - epsilon) {
//                // Zero sample points before start of reference data.
//                y[i] = 0;
//                i += di;
//            } else if(x[i] <= xref0[iref + diref] + epsilon) {
//                if(fabs(x[i] - xref0[iref]) < epsilon) {
//                    y[i] = yref[iref];
//                } else if(fabs(x[i] - xref0[iref + diref]) < epsilon) {
//                    y[i] = yref[iref + diref];
//                } else {
//                    // Interpolate (linear) our current sample point using the two encompassing reference data points.
//                    // Then go to the next sample point.
//                    dx = xref[iref + diref] - xref[iref];
//                    dy = yref[iref + diref] - yref[iref];
//                    y[i] = yref[iref] + (dy / dx) * (x[i] - xref0[iref]);
//                }
//                if(*firstPt == -1)
//                    *firstPt = i;
//                i += di;
//            } else {
//                // Increase reference data counter until our reference data points surround our current sample point.
//                iref += diref;
//            }
//        }
//        if(*firstPt != -1) {
//            if(isIncreasing) {
//                *numPts = i - *firstPt;
//            } else {
//                *numPts = *firstPt - i;
//                *firstPt = i + 1;
//            }
//        }
//        while(i >= 0 && i < n) {
//            // Zero sample points after end of reference data.
//            y[i] = 0;
//            i += di;
//        }
//        if(hasShiftedData)
//            delete [] xref0;
    }
    
    QObjectPropertyTreeSerializer::ObjectFactory getObjectFactory()
    {
        QObjectPropertyTreeSerializer::ObjectFactory factory;
        factory.registerCreator("StimulusClampProtocol::Stimulus", factory.defaultCreator<Stimulus>);
        factory.registerCreator("StimulusClampProtocol::Waveform", factory.defaultCreator<Waveform>);
        factory.registerCreator("StimulusClampProtocol::SimulationsSummary", factory.defaultCreator<SimulationsSummary>);
        factory.registerCreator("StimulusClampProtocol::ReferenceData", factory.defaultCreator<ReferenceData>);
        factory.registerCreator("StimulusClampProtocol::StimulusClampProtocol", factory.defaultCreator<StimulusClampProtocol>);
        return factory;
    }
    QObjectPropertyTreeSerializer::ObjectFactory StimulusClampProtocol::objectFactory = getObjectFactory();
    
    Eigen::VectorXd Stimulus::waveform(Eigen::VectorXd &time, int row, int col)
    {
        int numPts = time.size();
        Eigen::VectorXd stimulusWaveform = Eigen::VectorXd::Zero(numPts);
        double epsilon = std::numeric_limits<double>::epsilon() * 5;
        EigenLab::ParserXd parser;
        if(durations[row][col] > epsilon && fabs(amplitudes[row][col]) > epsilon) {
            for(int rep = 0; rep < repeats[row][col]; ++rep) {
                double onsetTime = starts[row][col] + rep * periods[row][col];
                double offsetTime = onsetTime + durations[row][col];
                Eigen::VectorXd::Index closestIndex;
                (time.array() - onsetTime).abs().minCoeff(&closestIndex);
                int firstOnsetPt = closestIndex;
                if(time[firstOnsetPt] < onsetTime - epsilon)
                    ++firstOnsetPt;
                if(firstOnsetPt < time.size()) {
                    (time.array() - offsetTime).abs().minCoeff(&closestIndex);
                    int firstOffsetPt = closestIndex;
                    if(time[firstOffsetPt] < offsetTime - epsilon)
                        ++firstOffsetPt;
                    int numOnsetPts = firstOffsetPt - firstOnsetPt;
                    int numOffsetPts = time.size() - firstOffsetPt;
                    if(onsetExprs[row][col].size() || offsetExprs[row][col].size()) {
                        if(numOnsetPts > 0 && onsetExprs[row][col].size()) {
                            try {
                                Eigen::VectorXd pulseTime = time.segment(firstOnsetPt, numOnsetPts).array() - onsetTime;
                                parser.var("t").setShared(pulseTime.data(), pulseTime.size(), 1);
                                stimulusWaveform.segment(firstOnsetPt, numOnsetPts)
                                += (parser.eval(onsetExprs[row][col]).matrix().array() * amplitudes[row][col]).matrix();
                            } catch(...) {
                            }
                        }
                        if(numOffsetPts > 0 && offsetExprs[row][col].size()) {
                            try {
                                Eigen::VectorXd pulseTime = time.segment(firstOffsetPt, numOffsetPts).array() - offsetTime;
                                parser.var("t").setShared(pulseTime.data(), pulseTime.size(), 1);
                                stimulusWaveform.segment(firstOffsetPt, numOffsetPts)
                                += (parser.eval(offsetExprs[row][col]).matrix().array() * amplitudes[row][col]).matrix();
                            } catch(...) {
                            }
                        }
                    } else {
                        // Square pulse.
                        if(numOnsetPts > 0)
                            stimulusWaveform.segment(firstOnsetPt, numOnsetPts) += Eigen::VectorXd::Constant(numOnsetPts, amplitudes[row][col]);
                    }
                }
            }
        }
        return stimulusWaveform;
    }
    
    void Simulation::findEpochsDiscretizedToSamplePoints()
    {
        epochs.clear();
        Epoch epoch;
        epoch.start = time[0];
        epoch.firstPt = 0;
        for(auto &kv : stimuli)
            epoch.stimuli[kv.first] = kv.second[0];
        epochs.push_back(epoch);
        int numPts = time.size();
        for(int i = 1; i < numPts; ++i) {
            for(auto &kv : stimuli) {
                if(kv.second[i] != kv.second[i - 1]) {
                    epochs.back().duration = time[i] - epochs.back().start;
                    epochs.back().numPts = i - epochs.back().firstPt;
                    epoch.start = time[i];
                    epoch.firstPt = i;
                    for(auto &kv2 : stimuli)
                        epoch.stimuli[kv2.first] = kv2.second[i];
                    epochs.push_back(epoch);
                    break;
                }
            }
        }
        epochs.back().duration = endTime - epochs.back().start;
        epochs.back().numPts = numPts - epochs.back().firstPt;
    }
    
    void Simulation::spectralSimulation(Eigen::RowVectorXd startingProbability, bool startEquilibrated, size_t variableSetIndex, AbortFlag *abort, QString */* message */)
    {
        int numPts = time.size();
        int numStates = startingProbability.size();
        while(probability.size() <= variableSetIndex)
            probability.push_back(Eigen::MatrixXd::Zero(numPts, numStates));
        Eigen::MatrixXd &P = probability.at(variableSetIndex);
        P.setZero(numPts, numStates);
        size_t epochCounter = 0;
        for(const Epoch &epoch : epochs) {
            if(abort && *abort) return;
            if(epochCounter == 0 && startEquilibrated) {
                // Set first epoch to equilibrium probabilities.
                startingProbability = startingProbability * epoch.uniqueEpoch->spectralMatrices[0];
                if(epoch.numPts > 0)
                    P.block(epoch.firstPt, 0, epoch.numPts, numStates).rowwise() = startingProbability;
            } else {
                if(epoch.numPts > 0) {
                    // Compute epoch probability using Q matrix spectral expansion.
                    Eigen::VectorXd epochTime = time.segment(epoch.firstPt, epoch.numPts).array() - epoch.start;
                    for(int i = 0; i < numStates; ++i) {
                        if(abort && *abort) return;
                        double lambda = epoch.uniqueEpoch->spectralEigenValues[i];
                        Eigen::MatrixXd &A = epoch.uniqueEpoch->spectralMatrices[i];
                        P.block(epoch.firstPt, 0, epoch.numPts, numStates) += (epochTime.array() * lambda).exp().matrix() * (startingProbability * A);
                    }
                }
                if(epochCounter + 1 < epochs.size()) {
                    // Update starting probability for next epoch.
                    Eigen::RowVectorXd temp = Eigen::RowVectorXd::Zero(numStates);
                    for(int i = 0; i < numStates; ++i) {
                        if(abort && *abort) return;
                        double lambda = epoch.uniqueEpoch->spectralEigenValues[i];
                        Eigen::MatrixXd &A = epoch.uniqueEpoch->spectralMatrices[i];
                        temp += ((startingProbability * A).array() * exp(lambda * epoch.duration)).matrix();
                    }
                    startingProbability = temp;
                }
            }
            ++epochCounter;
        }
    }
    
    void Simulation::monteCarloSimulation(Eigen::RowVectorXd startingProbability, std::mt19937 &randomNumberGenerator, size_t numRuns, bool accumulateRuns, bool sampleRuns, bool startEquilibrated, size_t variableSetIndex, AbortFlag *abort, QString *message)
    {
        int numStates = startingProbability.size();
        if(events.size() <= variableSetIndex)
            events.resize(1 + variableSetIndex);
        std::vector<MonteCarloEventChain> &eventChains = events.at(variableSetIndex);
        if(!accumulateRuns)
            eventChains.clear();
        size_t prevNumRuns = eventChains.size();
        eventChains.resize(prevNumRuns + numRuns);
        std::uniform_real_distribution<double> randomUniform(0, 1); // Uniform random numbers in [0, 1)
        double epsilon = std::numeric_limits<double>::epsilon() * 5;
        if(startEquilibrated)
            startingProbability = equilibriumProbability(epochs.begin()->uniqueEpoch->transitionRates.toDense());
        Eigen::SparseMatrix<double> QT; // Transpose of Q matrix. !!! Needs to be updated for each epoch.
        for(size_t run = prevNumRuns; run < prevNumRuns + numRuns; ++run) {
            if(abort && *abort) return;
            MonteCarloEventChain &eventChain = eventChains.at(run);
            eventChain.reserve(1000);
            size_t eventCounter = 0;
            MonteCarloEvent event;
            // Set starting state.
            event.state = -1;
            double prnd = randomUniform(randomNumberGenerator); // [0, 1)
            double ptot = 0;
            for(int i = 0; i < numStates; ++i) {
                ptot += startingProbability[i];
                if(ptot > prnd) {
                    event.state = i;
                    break;
                }
            }
            if(event.state == -1) {
                event.state = numStates - 1;
//                if(message) *message = "Failed to set starting state.";
//                if(abort) *abort = true;
//                return;
            }
            double eventChainDuration = 0;
            std::vector<Epoch>::iterator epochIter = epochs.begin();
            QT = epochIter->uniqueEpoch->transitionRates.transpose();
            while(eventChainDuration < endTime) {
                if(abort && *abort) return;
//                double lifetime = 0;
                // Check if stuck in state.
//                double kout = -epochIter->uniqueEpoch->transitionRates.coeff(event.state, event.state); // Net rate leaving state.
//                while(kout < epsilon) {
//                    lifetime = epochIter->start + epochIter->duration - eventChainDuration;
//                    // Go to next epoch.
//                    ++epochIter;
//                    if(epochIter == epochs.end())
//                        break;
//                    eventChainDuration = epochIter->start;
////                    event.duration = endTime - eventChainDuration; // Remaining time.
////                    eventChain.push_back(event);
////                    break;
//                }
                // Lifetime in state.
                double kout = -epochIter->uniqueEpoch->transitionRates.coeff(event.state, event.state); // Net rate leaving state.
                double lifetime = kout < epsilon ? endTime : epochIter->uniqueEpoch->randomStateLifetimes[event.state](randomNumberGenerator);
                bool epochChanged = false;
                while(eventChainDuration + lifetime > epochIter->start + epochIter->duration) { // Event takes us to the next epoch.
                    // Truncate lifetime to end of epoch.
                    lifetime = epochIter->start + epochIter->duration - eventChainDuration;
                    // Go to next epoch.
                    ++epochIter;
                    if(epochIter == epochs.end())
                        break;
                    // Check if stuck in state.
                    kout = -epochIter->uniqueEpoch->transitionRates.coeff(event.state, event.state); // Net rate leaving state.
//                    if(kout < epsilon) {
//                        epochIter = epochs.end();
//                        break;
//                    }
                    // Lifetime extends into new epoch.
                    lifetime += kout < epsilon ? endTime : epochIter->uniqueEpoch->randomStateLifetimes[event.state](randomNumberGenerator);
                    epochChanged = true;
                }
                // Check if we reached the end of the chain's duration.
                if(epochIter == epochs.end()) {
                    event.duration = endTime - eventChainDuration; // Remaining time.
                    eventChain.push_back(event);
                    break;
                }
                if(epochChanged)
                    QT = epochIter->uniqueEpoch->transitionRates.transpose();
                // Add event to chain.
                event.duration = lifetime;
                eventChain.push_back(event);
                eventChainDuration += lifetime;
                ++eventCounter;
                if(eventCounter == 1000) {
                    eventChain.reserve(eventChain.size() + 1000);
                    eventCounter = 0;
                }
                // Go to next state.
                if(eventChainDuration < endTime) {
                    // Select next state based on rates leaving current state.
                    prnd = randomUniform(randomNumberGenerator); // [0, 1)
                    ptot = 0;
                    for(Eigen::SparseMatrix<double>::InnerIterator it(QT, event.state); it; ++it) {
                        if(it.row() != event.state) {
                            ptot += it.value() / kout;
                            if(ptot >= prnd) {
                                event.state = it.row();
                                break;
                            }
                        }
                    }
                }
            }
        }
        if(sampleRuns) {
            size_t numPts = time.size();
            while(probability.size() <= variableSetIndex)
                probability.push_back(Eigen::MatrixXd::Zero(numPts, numStates));
            getProbabilityFromEventChains(probability.at(variableSetIndex), numStates, events.at(variableSetIndex), abort, message);
        }
    }
    
    void Simulation::getProbabilityFromEventChains(Eigen::MatrixXd &P, size_t numStates, const std::vector<MonteCarloEventChain> &eventChains, AbortFlag *abort, QString */* message */)
    {
        size_t numPts = time.size();
        P.setZero(numPts, numStates);
        for(const MonteCarloEventChain &eventChain : eventChains) {
            size_t t = 0; // Index into time.
            MonteCarloEventChain::const_iterator eventIter = eventChain.begin();
            double sampleIntervalStart = time[t];
            double sampleIntervalEnd = t + 1 >= numPts ? endTime : time[t + 1];
            double sampleInterval = sampleIntervalEnd - sampleIntervalStart;
            double eventStart = 0;
            double eventEnd = eventStart + eventIter->duration;
            while(t < numPts && eventIter != eventChain.end()) {
                if(abort && *abort) return;
                if(eventStart <= sampleIntervalStart && eventEnd >= sampleIntervalEnd) {
                    // Event covers entire sample interval.
                    P(t, eventIter->state) += 1;
                    ++t;
                    sampleIntervalStart = sampleIntervalEnd;
                    sampleIntervalEnd = t + 1 < numPts ? time[t + 1] : endTime;
                    sampleInterval = sampleIntervalEnd - sampleIntervalStart;
                } else if(eventStart <= sampleIntervalStart) {
                    // Event stopped mid sample interval.
                    P(t, eventIter->state) += (eventEnd - sampleIntervalStart) / sampleInterval;
                    ++eventIter;
                    if(eventIter == eventChain.end()) break;
                    eventStart = eventEnd;
                    eventEnd = eventStart + eventIter->duration;
                } else if(eventEnd >= sampleIntervalEnd) {
                    // Event started mid sample interval.
                    P(t, eventIter->state) += (sampleIntervalEnd - eventStart) / sampleInterval;
                    ++t;
                    sampleIntervalStart = sampleIntervalEnd;
                    sampleIntervalEnd = t + 1 < numPts ? time[t + 1] : endTime;
                    sampleInterval = sampleIntervalEnd - sampleIntervalStart;
                } else {
                    // Event started and stopped mid sample interval.
                    P(t, eventIter->state) += eventIter->duration / sampleInterval;
                    ++eventIter;
                    if(eventIter == eventChain.end()) break;
                    eventStart = eventEnd;
                    eventEnd = eventStart + eventIter->duration;
                }
            }
        }
        P /= eventChains.size();
    }
    
    double Simulation::maxProbabilityError()
    {
        double maxError = 0;
        for(size_t variableSetIndex = 0; variableSetIndex < probability.size(); ++variableSetIndex) {
            Eigen::MatrixXd &P = probability.at(variableSetIndex);
            double perr = (P.rowwise().sum().array() - 1).abs().maxCoeff();
            if(perr > maxError)
                maxError = perr;
        }
        return maxError;
    }
    
    QString ReferenceData::filePathRelativeToParentProtocol() const
    {
        if(StimulusClampProtocol *protocol = qobject_cast<StimulusClampProtocol*>(parent())) {
            return protocol->fileInfo().absoluteDir().relativeFilePath(filePath());
        }
        return filePath();
    }
    
    void ReferenceData::open(QString filePath)
    {
        if(filePath.isEmpty()) {
            filePath = QFileDialog::getOpenFileName(0, "Open reference data file...");
            if(filePath.isEmpty()) return;
        }
        QFileInfo fileInfo(filePath);
        StimulusClampProtocol *protocol = qobject_cast<StimulusClampProtocol*>(parent());
        if(fileInfo.isRelative() && protocol)
            filePath = protocol->fileInfo().absoluteDir().filePath(filePath);
        QFile file(filePath);
        if(!file.open(QIODevice::Text | QIODevice::ReadOnly)) {
            QMessageBox::information(0, "error", file.errorString() + ": " + filePath);
            return;
        }
        QTextStream in(&file);
        QString firstLine = in.readLine();
        QStringList colTitles = firstLine.split("\t", QString::SkipEmptyParts);
        int numColumns = colTitles.size();
        std::vector<std::vector<double> > colData;
        colData.resize(numColumns);
        for(int col = 0; col < numColumns; ++col)
            colData[col].reserve(10000);
        bool ok;
        while(!in.atEnd()) {
            QString line = in.readLine();
            QStringList fields = line.split(QRegExp("[ \t]"), QString::SkipEmptyParts);
            for(int col = 0; col < numColumns; ++col) {
                colData[col].push_back(col < fields.size() ? fields[col].toDouble(&ok) : 0);
                if(!ok) {
                    file.close();
                    QMessageBox::information(0, "error", "Non-numeric data '" + fields[col] + "'.");
                    return;
                }
            }
        }
        _fileInfo = QFileInfo(file);
        file.close();
        columnTitles = colTitles;
        columnData.resize(numColumns);
        for(int col = 0; col < numColumns; ++col) {
            int numRows = colData[col].size();
            columnData[col] = Eigen::VectorXd::Zero(numRows);
            for(int row = 0; row < numRows; ++row)
                columnData[col][row] = colData[col][row];
        }
        updateColumnPairsXY();
    }
    
    void ReferenceData::updateColumnPairsXY()
    {
        // Parse y(x) column pairs based on column titles.
        // Columns are either XYY... or XYXY...
        columnPairsXY.clear();
        if(columnData.size() == 0)
            return;
        if((columnData.size() % 2 == 0) && (columnTitles.size() > 2) && columnTitles[0] == columnTitles[2]) {
            for(int i = 0; i + 1 < int(columnData.size()); i += 2)
                columnPairsXY.push_back(std::pair<int, int>(i, i + 1));
        } else {
            for(int i = 1; i < int(columnData.size()); ++i)
                columnPairsXY.push_back(std::pair<int, int>(0, i));
        }
    }
    
    StimulusClampProtocol::StimulusClampProtocol(QObject *parent, const QString &name) :
    QObject(parent),
    _start("0"),
    _duration("1"),
    _sampleInterval("0.001"),
    _weight("1"),
    _startEquilibrated(false)
    {
        setName(name);
    }
    
    void StimulusClampProtocol::init(std::vector<Epoch*> &uniqueEpochs, const QStringList &stateNames)
    {
        this->stateNames = stateNames;
        QList<Stimulus*> stimuli = findChildren<Stimulus*>(QString(), Qt::FindDirectChildrenOnly);
        QList<SimulationsSummary*> summaries = findChildren<SimulationsSummary*>(QString(), Qt::FindDirectChildrenOnly);
        
        // Parse conditions matrices.
        starts = str2mat<double>(start());
        durations = str2mat<double>(duration());
        sampleIntervals = str2mat<double>(sampleInterval());
        weights = str2mat<double>(weight());
        foreach(Stimulus *stimulus, stimuli) {
            if(stimulus->isActive()) {
                stimulus->starts = str2mat<double>(stimulus->start());
                stimulus->durations = str2mat<double>(stimulus->duration());
                stimulus->amplitudes = str2mat<double>(stimulus->amplitude());
                stimulus->onsetExprs = str2mat<std::string>(stimulus->onsetExpr());
                stimulus->offsetExprs = str2mat<std::string>(stimulus->offsetExpr());
                stimulus->repeats = str2mat<int>(stimulus->repetitions());
                stimulus->periods = str2mat<double>(stimulus->period());
            }
        }
        foreach(SimulationsSummary *summary, summaries) {
            if(summary->isActive()) {
                summary->exprXs = str2mat<std::string>(summary->exprX());
                summary->exprYs = str2mat<std::string>(summary->exprY());
                summary->startXs = str2mat<double>(summary->startX());
                summary->durationXs = str2mat<double>(summary->durationX());
                summary->startYs = str2mat<double>(summary->startY());
                summary->durationYs = str2mat<double>(summary->durationY());
            }
        }
        // Get max size for all conditions matrices.
        size_t rows = 1, cols = 1;
        matlims<double>(starts, &rows, &cols);
        matlims<double>(durations, &rows, &cols);
        matlims<double>(sampleIntervals, &rows, &cols);
        matlims<double>(weights, &rows, &cols);
        foreach(Stimulus *stimulus, stimuli) {
            if(stimulus->isActive()) {
                matlims<double>(stimulus->starts, &rows, &cols);
                matlims<double>(stimulus->durations, &rows, &cols);
                matlims<double>(stimulus->amplitudes, &rows, &cols);
                matlims<std::string>(stimulus->onsetExprs, &rows, &cols);
                matlims<std::string>(stimulus->offsetExprs, &rows, &cols);
                matlims<int>(stimulus->repeats, &rows, &cols);
                matlims<double>(stimulus->periods, &rows, &cols);
            }
        }
        // Pad all conditions matrices out to max size.
        padmat<double>(starts, rows, cols, 0);
        padmat<double>(durations, rows, cols, 0);
        padmat<double>(sampleIntervals, rows, cols, 0);
        padmat<double>(weights, rows, cols, 1);
        foreach(Stimulus *stimulus, stimuli) {
            if(stimulus->isActive()) {
                padmat<double>(stimulus->starts, rows, cols, 0);
                padmat<double>(stimulus->durations, rows, cols, 0);
                padmat<double>(stimulus->amplitudes, rows, cols, 0);
                padmat<std::string>(stimulus->onsetExprs, rows, cols, "");
                padmat<std::string>(stimulus->offsetExprs, rows, cols, "");
                padmat<int>(stimulus->repeats, rows, cols, 1);
                padmat<double>(stimulus->periods, rows, cols, 0);
            }
        }
        foreach(SimulationsSummary *summary, summaries) {
            if(summary->isActive()) {
                padmat<std::string>(summary->exprXs, rows, cols, "");
                padmat<std::string>(summary->exprYs, rows, cols, "");
                padmat<double>(summary->startXs, rows, cols, 0);
                padmat<double>(summary->durationXs, rows, cols, 0);
                padmat<double>(summary->startYs, rows, cols, 0);
                padmat<double>(summary->durationYs, rows, cols, 0);
                // Sample indexes.
                summary->firstPtX = Eigen::MatrixXi::Zero(rows, cols);
                summary->numPtsX = Eigen::MatrixXi::Zero(rows, cols);
                summary->firstPtY = Eigen::MatrixXi::Zero(rows, cols);
                summary->numPtsY = Eigen::MatrixXi::Zero(rows, cols);
            }
        }
        // Init simulations for each condition.
        simulations.resize(rows);
        for(size_t row = 0; row < rows; ++row) {
            simulations[row].resize(cols);
            for(size_t col = 0; col < cols; ++col) {
                Simulation &sim = simulations[row][col];
                // Clear arrays.
                sim.probability.clear();
                sim.waveforms.clear();
                // Sample time points.
                int numSteps = floor(durations[row][col] / sampleIntervals[row][col]);
                sim.time = Eigen::VectorXd::LinSpaced(1 + numSteps, starts[row][col], starts[row][col] + numSteps * sampleIntervals[row][col]);
                sim.endTime = starts[row][col] + durations[row][col];
                int numPts = sim.time.size();
                // Sample weights.
                sim.weight = Eigen::VectorXd::Constant(numPts, weights[row][col]);
                // Stimulus waveforms (plus weight and mask).
                sim.stimuli.clear();
                Eigen::VectorXd mask = Eigen::VectorXd::Zero(numPts);
                foreach(Stimulus *stimulus, stimuli) {
                    if(stimulus->isActive()) {
                        if(stimulus->name().toLower() == "weight")
                            sim.weight += stimulus->waveform(sim.time, row, col);
                        else if(stimulus->name().toLower() == "mask")
                            mask += stimulus->waveform(sim.time, row, col);
                        else if(sim.stimuli.find(stimulus->name()) != sim.stimuli.end())
                            sim.stimuli[stimulus->name()] += stimulus->waveform(sim.time, row, col);
                        else
                            sim.stimuli[stimulus->name()] = stimulus->waveform(sim.time, row, col);
                    }
                }
                // Convert mask to boolean array (0, false = masked, 1, true = unmasked).
                sim.mask = (mask.array() == 0);
                // Stimulus epochs.
                sim.findEpochsDiscretizedToSamplePoints();
                // Unique epochs.
                for(Epoch &epoch : sim.epochs) {
                    bool foundIt = false;
                    for(Epoch *uniqueEpoch : uniqueEpochs) {
                        if(uniqueEpoch->stimuli == epoch.stimuli) {
                            epoch.uniqueEpoch = uniqueEpoch;
                            foundIt = true;
                            break;
                        }
                    }
                    if(!foundIt) {
                        epoch.uniqueEpoch = new Epoch;
                        epoch.uniqueEpoch->stimuli = epoch.stimuli;
                        uniqueEpochs.push_back(epoch.uniqueEpoch);
                    }
                }
                // Random number generator.
                sim.randomNumberGenerator = getSeededRandomNumberGenerator<std::mt19937>();
                // Summary sample indexes.
                foreach(SimulationsSummary *summary, summaries) {
                    if(summary->isActive()) {
                        int firstPt, numPts;
                        double startX = summary->startXs[row][col];
                        double stopX = startX + summary->durationXs[row][col];
                        findIndexesInRange(sim.time, startX, stopX, &firstPt, &numPts);
                        summary->firstPtX(row, col) = firstPt;
                        summary->numPtsX(row, col) = numPts;
                        double startY = summary->startYs[row][col];
                        double stopY = startY + summary->durationYs[row][col];
                        findIndexesInRange(sim.time, startY, stopY, &firstPt, &numPts);
                        summary->firstPtY(row, col) = firstPt;
                        summary->numPtsY(row, col) = numPts;
                    }
                }
                // Clear reference data.
                sim.referenceData.clear();
            } // col
        } // row
        // Reference data.
        QStringList summaryNames;
        foreach(SimulationsSummary *summary, summaries) {
            summaryNames.push_back(summary->name());
            summary->referenceData.clear();
        }
        foreach(ReferenceData *referenceData, findChildren<ReferenceData*>(QString(), Qt::FindDirectChildrenOnly)) {
            if(!summaryNames.contains(referenceData->name())) {
                size_t varSet = referenceData->variableSetIndex();
                size_t row = referenceData->rowIndex();
                size_t firstCol = referenceData->columnIndex();
                if(row < rows) {
                    for(size_t i = 0; i < referenceData->columnPairsXY.size() && firstCol + i < cols; ++i) {
                        size_t col = firstCol + i;
                        Simulation &sim = simulations[row][col];
                        if(sim.referenceData.size() <= varSet)
                            sim.referenceData.resize(varSet + 1);
                        Simulation::RefData refData;
                        refData.waveform = Eigen::VectorXd::Zero(sim.time.size());
                        int columnX = referenceData->columnPairsXY[i].first;
                        int columnY = referenceData->columnPairsXY[i].second;
                        Eigen::VectorXd &refX = referenceData->columnData[columnX];
                        Eigen::VectorXd &refY = referenceData->columnData[columnY];
                        double *xref = refX.data();
                        double *yref = refY.data();
                        int nref = refY.size();
                        double *x = sim.time.data();
                        double *y = refData.waveform.data();
                        int n = sim.time.size();
                        double epsilon = (sim.time.segment(1, n - 1) - sim.time.segment(0, n - 1)).minCoeff() * 1e-5;
                        double epsilonRef = (refX.segment(1, nref - 1) - refX.segment(0, nref - 1)).minCoeff() * 1e-5;
                        if(epsilonRef < epsilon)
                            epsilon = epsilonRef;
                        sampleArray(xref, yref, nref, x, y, n, &refData.firstPt, &refData.numPts, referenceData->x0(), epsilon);
                        if(refData.numPts > 0) {
                            refData.waveform = refData.waveform.segment(refData.firstPt, refData.numPts);
                            if(referenceData->normalization() == ReferenceData::ToMax) {
                                refData.waveform /= refData.waveform.maxCoeff();
                            } else if(referenceData->normalization() == ReferenceData::ToMin) {
                                refData.waveform /= refData.waveform.minCoeff();
                            } else if(referenceData->normalization() == ReferenceData::ToAbsMinMax) {
                                double min = refData.waveform.minCoeff();
                                double max = refData.waveform.maxCoeff();
                                double peak = (fabs(max) >= fabs(min) ? max : min);
                                refData.waveform /= peak;
                            }
                            if(referenceData->scale() != 1)
                                refData.waveform *= referenceData->scale();
                            refData.weight = referenceData->weight();
                            sim.referenceData.at(varSet)[referenceData->name()] = refData;
                        }
                    } // i
                }
            }
        } // referenceData
    }
    
    double StimulusClampProtocol::cost()
    {
        double cost = 0;
        for(size_t row = 0; row < simulations.size(); ++row) {
            for(size_t col = 0; col < simulations[row].size(); ++col) {
                Simulation &sim = simulations[row][col];
                for(size_t variableSetIndex = 0; variableSetIndex < sim.referenceData.size(); ++variableSetIndex) {
                    for(auto &kv : sim.referenceData.at(variableSetIndex)) {
                        Simulation::RefData &refData = kv.second;
                        if(refData.numPts > 0) {
                            double *x = 0;
                            double *y = 0;
                            int n;
                            getSimulationWaveform(kv.first, sim, variableSetIndex, &x, &y, &n);
                            if(x && y && n > 0) {
                                Eigen::Map<Eigen::VectorXd> data(y + refData.firstPt, refData.numPts);
                                Eigen::Map<Eigen::VectorXd> weight(sim.weight.data() + refData.firstPt, refData.numPts);
                                cost += ((data - refData.waveform).array().pow(2) * weight.array()).sum() * refData.weight;
                            }
                        }
                    }
                }
            }
        }
        foreach(SimulationsSummary *summary, findChildren<SimulationsSummary*>(QString(), Qt::FindDirectChildrenOnly)) {
            if(summary->isActive()) {
                for(size_t variableSetIndex = 0; variableSetIndex < summary->referenceData.size(); ++variableSetIndex) {
                    for(size_t row = 0; row < summary->referenceData.at(variableSetIndex).size(); ++row) {
                        SimulationsSummary::RefData &refData = summary->referenceData.at(variableSetIndex).at(row);
                        if(refData.numPts > 0) {
                            SimulationsSummary::RowMajorMatrixXd &dataY = summary->dataY.at(variableSetIndex);
                            Eigen::Map<Eigen::RowVectorXd> data(dataY.row(row).data() + refData.firstPt, refData.numPts);
                            cost += ((data - refData.waveform).array().pow(2)).sum() * refData.weight;
                        }
                    }
                }
            }
        }
        return cost;
    }
    
    void StimulusClampProtocol::getSimulationWaveform(const QString &name, Simulation &sim, size_t variableSetIndex, double **x, double **y, int *n)
    {
        int stateIndex = stateNames.indexOf(name);
        if(stateIndex != -1) {
            if(sim.probability.size() > variableSetIndex) {
                *x = sim.time.data();
                *y = sim.probability.at(variableSetIndex).col(stateIndex).data();
                *n = sim.time.size();
            }
            return;
        }
        std::map<QString, Eigen::VectorXd>::iterator stimulusIter = sim.stimuli.find(name);
        if(stimulusIter != sim.stimuli.end()) {
            *x = sim.time.data();
            *y = stimulusIter->second.data();
            *n = sim.time.size();
            return;
        }
        if(sim.waveforms.size() > variableSetIndex) {
            std::map<QString, Eigen::VectorXd>::iterator waveformIter = sim.waveforms.at(variableSetIndex).find(name);
            if(waveformIter != sim.waveforms.at(variableSetIndex).end()) {
                *x = sim.time.data();
                *y = waveformIter->second.data();
                *n = sim.time.size();
                return;
            }
        }
    }
    
    void StimulusClampProtocol::getSimulationRefWaveform(const QString &name, Simulation &sim, size_t variableSetIndex, double **x, double **y, int *n)
    {
        if(sim.referenceData.size() > variableSetIndex) {
            std::map<QString, Simulation::RefData>::iterator refIter = sim.referenceData.at(variableSetIndex).find(name);
            if(refIter != sim.referenceData.at(variableSetIndex).end()) {
                Simulation::RefData &refData = refIter->second;
                *x = sim.time.data() + refData.firstPt;
                *y = refData.waveform.data();
                *n = refData.numPts;
                return;
            }
        }
    }
    
    void StimulusClampProtocol::getSummaryWaveform(const QString &name, size_t variableSetIndex, size_t row, double **x, double **y, int *n, QString *xExpr, QString *yExpr)
    {
        foreach(SimulationsSummary *summary, findChildren<SimulationsSummary*>(QString(), Qt::FindDirectChildrenOnly)) {
            if(summary->isActive() && summary->name() == name) {
                if(summary->dataX.size() > variableSetIndex && summary->dataY.size() > variableSetIndex) {
                    *x = summary->dataX.at(variableSetIndex).row(row).data();
                    *y = summary->dataY.at(variableSetIndex).row(row).data();
                    *n = summary->dataY.at(variableSetIndex).cols();
                    if(xExpr) *xExpr = QString::fromStdString(summary->exprXs.at(row).at(0));
                    if(yExpr) *yExpr = QString::fromStdString(summary->exprYs.at(row).at(0));
                }
                return;
            }
        }
    }
    
    void StimulusClampProtocol::getSummaryRefWaveform(const QString &name, size_t variableSetIndex, size_t row, double **x, double **y, int *n, QString *xExpr, QString *yExpr)
    {
        foreach(SimulationsSummary *summary, findChildren<SimulationsSummary*>(QString(), Qt::FindDirectChildrenOnly)) {
            if(summary->isActive() && summary->name() == name) {
                if(summary->referenceData.size() > variableSetIndex && summary->referenceData.at(variableSetIndex).size() > row) {
                    SimulationsSummary::RefData &refData = summary->referenceData.at(variableSetIndex).at(row);
                    *x = summary->dataX.at(variableSetIndex).row(row).data() + refData.firstPt;
                    *y = refData.waveform.data();
                    *n = refData.numPts;
                    if(xExpr) *xExpr = QString::fromStdString(summary->exprXs.at(row).at(0));
                    if(yExpr) *yExpr = QString::fromStdString(summary->exprYs.at(row).at(0));
                }
                return;
            }
        }
    }
    
#ifdef DEBUG
    void StimulusClampProtocol::dump(std::ostream &out)
    {
        QVariantMap data = QObjectPropertyTreeSerializer::serialize(this, 1, true, false);
        out << QJsonDocument::fromVariant(data).toJson(QJsonDocument::Indented).toStdString();
    }
#endif
    
    void StimulusClampProtocol::clear()
    {
        qDeleteAll(children());
        simulations.clear();
    }
    
    void StimulusClampProtocol::open(QString filePath)
    {
        if(filePath.isEmpty())
            filePath = QFileDialog::getOpenFileName(0, "Open Stimulus Clamp Protocol...", _fileInfo.absoluteFilePath());
        if(filePath.isEmpty())
            return;
        QFile file(filePath);
        if(!file.open(QIODevice::Text | QIODevice::ReadOnly))
            return;
        QString buffer = file.readAll();
        _fileInfo = QFileInfo(file);
        file.close();
        QVariantMap data = QJsonDocument::fromJson(buffer.toUtf8()).toVariant().toMap();
        if(data.contains("StimulusClampProtocol::StimulusClampProtocol"))
            QObjectPropertyTreeSerializer::deserialize(this, data["StimulusClampProtocol::StimulusClampProtocol"].toMap(), &objectFactory);
    }
    
    void StimulusClampProtocol::save()
    {
        saveAs(_fileInfo.absoluteFilePath());
    }
    
    void StimulusClampProtocol::saveAs(QString filePath)
    {
        if(filePath.isEmpty())
            filePath = QFileDialog::getSaveFileName(0, "Save Stimulus Clamp Protocol...", _fileInfo.absoluteFilePath());
        if(filePath.isEmpty())
            return;
        QFile file(filePath);
        if(!file.open(QIODevice::Text | QIODevice::WriteOnly))
            return;
        QTextStream out(&file);
        QVariantMap data;
        data["StimulusClampProtocol::StimulusClampProtocol"] = QObjectPropertyTreeSerializer::serialize(this, -1, true, false);
        out << QJsonDocument::fromVariant(data).toJson(QJsonDocument::Indented);
        _fileInfo = QFileInfo(file);
        file.close();
    }
    
    void StimulusClampProtocol::saveMonteCarloEventChainsAsDwt(QString filePath)
    {
        // Segment: 1 Dwells: 2 Sampling(ms): 1
        // 0    134
        // 1    27
        //
        // Segment: 2 Dwells: 3 Sampling(ms): 1
        // 0    77
        // 1    21
        // 0    56
        if(filePath.isEmpty())
            filePath = QFileDialog::getSaveFileName(0, "Save Monte Carlo event chains (*.dwt)...");
        if(filePath.endsWith(".dwt"))
            filePath.chop(4);
        if(filePath.isEmpty())
            return;
        for(size_t row = 0; row < simulations.size(); ++row) {
            for(size_t col = 0; col < simulations[row].size(); ++col) {
                Simulation &sim = simulations[row][col];
                for(size_t variableSetIndex = 0; variableSetIndex < sim.events.size(); ++variableSetIndex) {
                    QFile file(filePath + " (" + QString::number(variableSetIndex) + "," + QString::number(row) + "," + QString::number(col) + ").dwt");
                    if(!file.open(QIODevice::Text | QIODevice::WriteOnly))
                        return;
                    QTextStream out(&file);
                    int segment = 1;
                    foreach(const MonteCarloEventChain &eventChain, sim.events.at(variableSetIndex)) {
                        out << "Segment: " << segment << " Dwells: " << eventChain.size() - 1 << " Sampling(ms): 1\r\n";
                        foreach(const MonteCarloEvent &event, eventChain)
                            out << event.state << "\t" << event.duration * 1000 << "\r\n";
                        out << "\r\n";
                        ++segment;
                    }
                    file.close();
                }
            }
        }
    }
    
    StimulusClampProtocolSimulator::StimulusClampProtocolSimulator(const QString &labelText, QWidget *parent) :
    QProgressDialog(labelText, "Abort", 0, 0, parent),
    model(0),
    abort(false),
    minimizer(0),
    x(0),
    dx(0)
    {
        connect(this, SIGNAL(canceled()), this, SLOT(_abort()));
        connect(&_watcher, SIGNAL(finished()), this, SLOT(_finish()));
        connect(this, SIGNAL(iterationChanged(int)), this, SLOT(_atIteration(int)));
        setWindowModality(Qt::WindowModality::ApplicationModal);
    }
    
    StimulusClampProtocolSimulator::~StimulusClampProtocolSimulator()
    {
        for(Epoch *epoch : uniqueEpochs) delete epoch;
        if(minimizer) gsl_multimin_fminimizer_free(minimizer);
        if(x) gsl_vector_free(x);
        if(dx) gsl_vector_free(dx);
    }
    
    void StimulusClampProtocolSimulator::simulate(bool showProgressDialog)
    {
        setRange(0, 0); // Infinite wait progress bar.
        if(showProgressDialog)
            QTimer::singleShot(2000, this, SLOT(show())); // Show dialog after 2000 ms.
        try {
            initSimulation();
            _future = QtConcurrent::run(static_cast<StimulusClampProtocolSimulator*>(this), &StimulusClampProtocolSimulator::runSimulation);
            _watcher.setFuture(_future);
        } catch(std::runtime_error &e) {
            message = QString(e.what());
            _finish();
        } catch(...) {
            message = "Undocumentded error.";
            _finish();
        }
    }
    
    void StimulusClampProtocolSimulator::initSimulation()
    {
        model->init(stateNames);
        for(Epoch *epoch : uniqueEpochs)
            delete epoch;
        uniqueEpochs.clear();
        foreach(StimulusClampProtocol *protocol, protocols)
            protocol->init(uniqueEpochs, stateNames);
    }
    
    void StimulusClampProtocolSimulator::runSimulation()
    {
        try {
            QList<MarkovModel::StateGroup*> stateGroups = model->findChildren<MarkovModel::StateGroup*>(QString(), Qt::FindDirectChildrenOnly);
            QString method = options["Method"].toString();
            std::vector<QFuture<void> > futures;
            for(size_t variableSetIndex = 0; variableSetIndex < model->numVariableSets(); ++variableSetIndex) {
                // Unique epochs.
                for(Epoch *epoch : uniqueEpochs) {
                    if(abort) break;
                    model->evalVariables(epoch->stimuli, variableSetIndex);
                    model->getStateProbabilities(epoch->stateProbabilities);
                    model->getStateAttributes(epoch->stateAttributes);
                    model->getTransitionRates(epoch->transitionRates);
                    model->getTransitionCharges(epoch->transitionCharges);
                    int numStates = epoch->transitionRates.cols();
                    if(method == "Eigen Solver") {
                        std::function<void()> func = std::bind(spectralExpansion, std::ref(epoch->transitionRates), std::ref(epoch->spectralEigenValues), std::ref(epoch->spectralMatrices), &abort);
                        futures.push_back(QtConcurrent::run(func));
                    } else if(method == "Monte Carlo") {
                        epoch->spectralEigenValues = Eigen::VectorXd::Zero(1);
                        epoch->spectralMatrices.clear();
                        epoch->randomStateLifetimes.clear();
                        epoch->randomStateLifetimes.reserve(numStates);
                        for(int i = 0; i < numStates; ++i)
                            epoch->randomStateLifetimes.push_back(std::exponential_distribution<double>(-epoch->transitionRates.coeff(i, i)));
                    }
                    if(epoch->transitionCharges.nonZeros())
                        epoch->stateChargeCurrents = epoch->transitionRates.cwiseProduct(epoch->transitionCharges).toDense().rowwise().sum() * 6.242e-6; // pA = 6.242e-6 e/s
                    else
                        epoch->stateChargeCurrents = Eigen::RowVectorXd::Zero(numStates);
                } // epoch
                for(QFuture<void> &future : futures)
                    future.waitForFinished();
                futures.clear();
                // Simulations.
                int numRuns = options.contains("# Monte Carlo runs") ? options["# Monte Carlo runs"].toInt() : 0;
                bool accumulateRuns = options.contains("Accumulate Monte Carlo runs") ? options["Accumulate Monte Carlo runs"].toBool() : false;
                bool sampleRuns = options.contains("Sample probability from Monte Carlo event chains") ? options["Sample probability from Monte Carlo event chains"].toBool() : true;
                for(StimulusClampProtocol *protocol : protocols) {
                    for(size_t row = 0; row < protocol->simulations.size(); ++row) {
                        for(size_t col = 0; col < protocol->simulations[row].size(); ++col) {
                            if(abort) break;
                            Simulation &sim = protocol->simulations[row][col];
                            if(method == "Eigen Solver") {
                                std::function<void()> func = std::bind(&Simulation::spectralSimulation, &sim, sim.epochs.begin()->uniqueEpoch->stateProbabilities, protocol->startEquilibrated(), variableSetIndex, &abort, &message);
                                futures.push_back(QtConcurrent::run(func));
                            } else if(method == "Monte Carlo") {
                                std::function<void()> func = std::bind(&Simulation::monteCarloSimulation, &sim, sim.epochs.begin()->uniqueEpoch->stateProbabilities, std::ref(sim.randomNumberGenerator), numRuns, accumulateRuns, sampleRuns, protocol->startEquilibrated(), variableSetIndex, &abort, &message);
                                futures.push_back(QtConcurrent::run(func));
                            }
                        } // col
                    } // row
                } // protocol
                for(QFuture<void> &future : futures)
                    future.waitForFinished();
                futures.clear();
                // State groups, waveforms and summaries.
                EigenLab::ParserXd parser;
                for(StimulusClampProtocol *protocol : protocols) {
                    size_t rows = protocol->simulations.size();
                    size_t cols = rows ? protocol->simulations[0].size() : 0;
                    // Allocate memory for summaries.
                    QList<SimulationsSummary*> summaries = protocol->findChildren<SimulationsSummary*>(QString(), Qt::FindDirectChildrenOnly);
                    foreach(SimulationsSummary *summary, summaries) {
                        if(summary->isActive()) {
                            while(summary->dataX.size() <= variableSetIndex)
                                summary->dataX.push_back(SimulationsSummary::RowMajorMatrixXd::Zero(rows, cols));
                            while(summary->dataY.size() <= variableSetIndex)
                                summary->dataY.push_back(SimulationsSummary::RowMajorMatrixXd::Zero(rows, cols));
                            SimulationsSummary::RowMajorMatrixXd &dataX = summary->dataX.at(variableSetIndex);
                            SimulationsSummary::RowMajorMatrixXd &dataY = summary->dataY.at(variableSetIndex);
                            dataX = SimulationsSummary::RowMajorMatrixXd::Zero(rows, cols);
                            dataY = SimulationsSummary::RowMajorMatrixXd::Zero(rows, cols);
                            if(summary->referenceData.size() < model->numVariableSets())
                                summary->referenceData.resize(model->numVariableSets());
                            if(summary->referenceData.at(variableSetIndex).size() < rows)
                                summary->referenceData.at(variableSetIndex).resize(rows);
                            for(size_t row = 0; row < rows; ++row)
                                summary->referenceData.at(variableSetIndex).at(row).numPts = 0;
                        }
                    }
                    for(size_t row = 0; row < rows; ++row) {
                        for(size_t col = 0; col < cols; ++col) {
                            if(abort) break;
                            Simulation &sim = protocol->simulations[row][col];
                            int numPts = sim.time.size();
                            int numStates = sim.epochs.begin()->uniqueEpoch->transitionRates.cols();
                            // Probability ptr.
                            Eigen::MatrixXd *probability = 0;
                            if(sim.probability.size() > variableSetIndex)
                                probability = &sim.probability.at(variableSetIndex);
                            if(probability && (probability->rows() != numPts || probability->cols() != numStates))
                                probability = 0;
                            Eigen::MatrixXd tempProbability;
                            if(!probability && method == "Monte Carlo" && sim.events.size() > variableSetIndex) {
                                sim.getProbabilityFromEventChains(tempProbability, numStates, sim.events.at(variableSetIndex), &abort, &message);
                                probability = &tempProbability;
                            }
                            // Waveforms ref.
                            if(sim.waveforms.size() < model->numVariableSets())
                                sim.waveforms.resize(model->numVariableSets());
                            std::map<QString, Eigen::VectorXd> &waveforms = sim.waveforms.at(variableSetIndex);
                            // State attributes.
                            if(probability) {
                                for(Epoch &epoch : sim.epochs) {
                                    for(auto &kv : epoch.uniqueEpoch->stateAttributes) {
                                        QString attrName = kv.first;
                                        Eigen::RowVectorXd &stateAttrValues = kv.second;
                                        if(waveforms.find(attrName) == waveforms.end())
                                            waveforms[attrName] = Eigen::VectorXd::Zero(numPts);
                                        waveforms[attrName].segment(epoch.firstPt, epoch.numPts) = probability->block(epoch.firstPt, 0, epoch.numPts, numStates) * stateAttrValues.transpose();
                                    }
                                }
                            }
                            // Parser
                            parser.vars().clear();
                            for(auto &kv : model->parameters)
                                parser.var(kv.first.toStdString()).setLocal(kv.second);
                            parser.var("t").setShared(sim.time);
                            for(auto &kv : sim.stimuli)
                                parser.var(kv.first.toStdString()).setShared(kv.second.data(), numPts, 1);
                            if(probability) {
                                for(int i = 0; i < numStates; ++i)
                                    parser.var(stateNames.at(i).toStdString()).setShared(probability->col(i).data(), numPts, 1);
                            }
                            for(auto &kv : waveforms)
                                parser.var(kv.first.toStdString()).setShared(kv.second.data(), numPts, 1);
                            // State groups.
                            if(probability) {
                                foreach(MarkovModel::StateGroup *stateGroup, stateGroups) {
                                    if(stateGroup->isActive()) {
                                        waveforms[stateGroup->name()] = Eigen::VectorXd::Zero(numPts);
                                        Eigen::VectorXd &waveform = waveforms.at(stateGroup->name());
                                        for(int stateIndex : stateGroup->stateIndexes)
                                            waveform += probability->col(stateIndex);
                                        parser.var(stateGroup->name().toStdString()).setShared(waveform.data(), numPts, 1);
                                    }
                                }
                            }
                            // Waveforms.
                            foreach(Waveform *waveform, protocol->findChildren<Waveform*>(QString(), Qt::FindDirectChildrenOnly)) {
                                if(abort) break;
                                if(waveform->isActive()) {
                                    EigenLab::ValueXd result = parser.eval(waveform->expr().toStdString());
                                    if(result.matrix().rows() != numPts || result.matrix().cols() != 1)
                                        throw std::runtime_error("Invalid dimensions for waveform '" + waveform->expr().toStdString() + "'.");
                                    waveforms[waveform->name()] = result.matrix();
                                    parser.var(waveform->name().toStdString()).setShared(waveforms.at(waveform->name()).data(), numPts, 1);
                                }
                            }
                            // Summaries.
                            foreach(SimulationsSummary *summary, summaries) {
                                if(abort) break;
                                if(summary->isActive()) {
                                    SimulationsSummary::RowMajorMatrixXd &dataX = summary->dataX.at(variableSetIndex);
                                    SimulationsSummary::RowMajorMatrixXd &dataY = summary->dataY.at(variableSetIndex);
                                    // Limit parser to summary range.
                                    int firstPt = summary->firstPtX(row, col);
                                    int numPts = summary->numPtsX(row, col);
                                    parser.vars().clear();
                                    for(auto &kv : model->parameters)
                                        parser.var(kv.first.toStdString()).setLocal(kv.second);
                                    parser.var("t").setShared(sim.time.data() + firstPt, numPts, 1);
                                    for(auto &kv : sim.stimuli)
                                        parser.var(kv.first.toStdString()).setShared(kv.second.data() + firstPt, numPts, 1);
                                    if(probability) {
                                        for(int i = 0; i < numStates; ++i)
                                            parser.var(stateNames.at(i).toStdString()).setShared(probability->col(i).data() + firstPt, numPts, 1);
                                    }
                                    for(auto &kv : waveforms)
                                        parser.var(kv.first.toStdString()).setShared(kv.second.data() + firstPt, numPts, 1);
                                    // Evaluate summary expression.
                                    EigenLab::ValueXd result = parser.eval(summary->exprXs[row][col]);
                                    if(result.matrix().size() != 1)
                                        throw std::runtime_error("Summary '" + summary->exprXs[row][col] + "' does not reduce to a single value.");
                                    dataX(row, col) = result.matrix()(0, 0);
                                    // Limit parser to summary range.
                                    if(summary->firstPtY(row, col) != firstPt || summary->numPtsY(row, col) != numPts) {
                                        firstPt = summary->firstPtY(row, col);
                                        numPts = summary->numPtsY(row, col);
                                        parser.vars().clear();
                                        for(auto &kv : model->parameters)
                                            parser.var(kv.first.toStdString()).setLocal(kv.second);
                                        parser.var("t").setShared(sim.time.data() + firstPt, numPts, 1);
                                        for(auto &kv : sim.stimuli)
                                            parser.var(kv.first.toStdString()).setShared(kv.second.data() + firstPt, numPts, 1);
                                        if(probability) {
                                            for(int i = 0; i < numStates; ++i)
                                                parser.var(stateNames.at(i).toStdString()).setShared(probability->col(i).data() + firstPt, numPts, 1);
                                        }
                                        for(auto &kv : waveforms)
                                            parser.var(kv.first.toStdString()).setShared(kv.second.data() + firstPt, numPts, 1);
                                    }
                                    // Evaluate summary expression.
                                    result = parser.eval(summary->exprYs[row][col]);
                                    if(result.matrix().size() != 1)
                                        throw std::runtime_error("Summary '" + summary->exprYs[row][col] + "' does not reduce to a single value.");
                                    dataY(row, col) = result.matrix()(0, 0);
                                } // isActive
                            } // summary
                        } // col
                    } // row
                    // Summary normalization.
                    foreach(SimulationsSummary *summary, summaries) {
                        if(summary->isActive()) {
                            SimulationsSummary::RowMajorMatrixXd &dataY = summary->dataY.at(variableSetIndex);
                            if(summary->normalization() == SimulationsSummary::PerRow) {
                                for(int row = 0; row < dataY.rows(); ++row)
                                    dataY.row(row) /= dataY.row(row).array().abs().maxCoeff();
                            } else if(summary->normalization() == SimulationsSummary::AllRows) {
                                dataY /= dataY.array().abs().maxCoeff();
                            }
                        }
                    }
                } // protocol
            } // variableSetIndex
            // Summary reference data.
            for(StimulusClampProtocol *protocol : protocols) {
                size_t rows = protocol->simulations.size();
                size_t cols = rows ? protocol->simulations[0].size() : 0;
                foreach(ReferenceData *referenceData, protocol->findChildren<ReferenceData*>(QString(), Qt::FindDirectChildrenOnly)) {
                    size_t varSet = referenceData->variableSetIndex();
                    size_t firstRow = referenceData->rowIndex();
                    foreach(SimulationsSummary *summary, protocol->findChildren<SimulationsSummary*>(QString(), Qt::FindDirectChildrenOnly)) {
                        if(summary->isActive() && summary->name() == referenceData->name()) {
                            SimulationsSummary::RowMajorMatrixXd &dataX = summary->dataX.at(varSet);
                            for(size_t i = 0; i < referenceData->columnPairsXY.size() && firstRow + i < rows; ++i) {
                                size_t row = firstRow + i;
                                //qDebug() << "summary " << summary->name() << "ref data for row" << row;
                                SimulationsSummary::RefData &refData = summary->referenceData.at(varSet).at(row);
                                refData.waveform = Eigen::RowVectorXd::Zero(cols);
                                int columnX = referenceData->columnPairsXY[i].first;
                                int columnY = referenceData->columnPairsXY[i].second;
                                Eigen::VectorXd &refX = referenceData->columnData[columnX];
                                Eigen::VectorXd &refY = referenceData->columnData[columnY];
                                double *xref = refX.data();
                                double *yref = refY.data();
                                int nref = refY.size();
                                double *x = dataX.row(row).data();
                                double *y = refData.waveform.data();
                                int n = cols;
                                double epsilon = (dataX.row(row).segment(1, n - 1) - dataX.row(row).segment(0, n - 1)).array().abs().minCoeff() * 1e-5;
                                double epsilonRef = (refX.segment(1, nref - 1) - refX.segment(0, nref - 1)).array().abs().minCoeff() * 1e-5;
                                if(epsilonRef < epsilon)
                                    epsilon = epsilonRef;
                                //std::cout << "refX: " << refX << std::endl;
                                //std::cout << "refY: " << refY << std::endl;
                                //std::cout << "datX: " << dataX.row(row) << std::endl;
                                sampleArray(xref, yref, nref, x, y, n, &refData.firstPt, &refData.numPts, referenceData->x0(), epsilon);
                                //std::cout << "datY: " << refData.waveform << std::endl;
                                //qDebug() << "indexes: " << refData.firstPt << refData.numPts;
                                if(refData.numPts > 0) {
                                    refData.waveform = refData.waveform.segment(refData.firstPt, refData.numPts);
                                    if(referenceData->normalization() == ReferenceData::ToMax) {
                                        refData.waveform /= refData.waveform.maxCoeff();
                                    } else if(referenceData->normalization() == ReferenceData::ToMin) {
                                        refData.waveform /= refData.waveform.minCoeff();
                                    } else if(referenceData->normalization() == ReferenceData::ToAbsMinMax) {
                                        double min = refData.waveform.minCoeff();
                                        double max = refData.waveform.maxCoeff();
                                        double peak = (fabs(max) >= fabs(min) ? max : min);
                                        refData.waveform /= peak;
                                    }
                                    if(referenceData->scale() != 1)
                                        refData.waveform *= referenceData->scale();
                                    refData.weight = referenceData->weight();
                                }
                            }
                            break;
                        }
                    } // summary
                } // referenceData
            } // protocol
        } catch(std::runtime_error &e) {
            abort = true;
            message = QString(e.what());
            throw std::runtime_error(message.toStdString());
        } catch(...) {
            abort = true;
            message = "Undocumentded error.";
            throw std::runtime_error(message.toStdString());
        }
    }
    
    void StimulusClampProtocolSimulator::optimize(size_t maxIterations, double tolerance, bool showProgressDialog)
    {
        setRange(0, maxIterations); // Wait progress bar.
        setValue(0);
        if(showProgressDialog)
            show();
        try {
            initOptimization();
            _future = QtConcurrent::run(static_cast<StimulusClampProtocolSimulator*>(this), &StimulusClampProtocolSimulator::runOptimization, maxIterations, tolerance);
            _watcher.setFuture(_future);
        } catch(std::runtime_error &e) {
            message = QString(e.what());
            _finish();
        } catch(...) {
            message = "Undocumentded error.";
            _finish();
        }
    }
    
    double costFunctionForOptimizer(const gsl_vector *x, void *params)
    {
        StimulusClampProtocolSimulator *optimizer = static_cast<StimulusClampProtocolSimulator*>(params);
        int n = x->size;
        std::vector<double> vars(n);
        for(int i = 0; i < n; ++i)
            vars[i] = optimizer->angular2linear(gsl_vector_get(x, i), optimizer->xmin[i], optimizer->xmax[i]);
        optimizer->model->setFreeVariables(vars);
        optimizer->runSimulation();
        return optimizer->cost();
    }
    
    void StimulusClampProtocolSimulator::initOptimization()
    {
        initSimulation();
        if(!model) return;
        model->getFreeVariables(x0, xmin, xmax);
        if(x0.size() == 0)
            throw std::runtime_error("No variables to optimize.");
        int n = x0.size();
        x = gsl_vector_alloc(n);
        dx = gsl_vector_alloc(n);
        for(int i = 0; i < n; ++i) {
            gsl_vector_set(x, i, linear2angular(x0[i], xmin[i], xmax[i]));
            gsl_vector_set(dx, i, M_PI / 50);
        }
        minimizer = gsl_multimin_fminimizer_alloc(gsl_multimin_fminimizer_nmsimplex2, n);
        func.n = n;
        func.f = costFunctionForOptimizer;
        func.params = this;
        gsl_multimin_fminimizer_set(minimizer, &func, x, dx);
    }
    
    void StimulusClampProtocolSimulator::runOptimization(size_t maxIterations, double tolerance)
    {
        for(size_t i = 0; i < maxIterations; ++i) {
            if(i % 2 == 0)
                emit iterationChanged(i);
            int status = gsl_multimin_fminimizer_iterate(minimizer);
            if(status == 0) {
                double size = gsl_multimin_fminimizer_size(minimizer);
                status = gsl_multimin_test_size(size, tolerance);
            }
            if(status != GSL_CONTINUE || abort)
                break;
        }
        // Apply minimized parameters by calling cost function.
        (*(func.f))(minimizer->x, func.params);
        emit iterationChanged(maxIterations);
    }
    
    double StimulusClampProtocolSimulator::cost()
    {
        double cost = 0;
        for(StimulusClampProtocol *protocol : protocols)
            cost += protocol->cost();
        return cost;
    }
    
    void StimulusClampProtocolSimulator::_abort()
    {
        abort = true;
        emit aborted();
        // Redraw dialog with "Aborting..." message.
        setLabelText("Aborting...");
        show();
        QApplication::processEvents();
    }
    
    void StimulusClampProtocolSimulator::_finish()
    {
        emit finished();
        if(!message.isEmpty()) {
            show();
            QErrorMessage errMsg(this);
            errMsg.showMessage(message);
            errMsg.exec();
        }
        close();
        deleteLater();
    }
    
    // Specialization for strings because ranges don't make sense for strings.
    template <>
    std::vector<std::string> str2vec<std::string>(const QString &str, const QString &delimiterRegex, const QString &/* rangeDelimiterRegex */)
    {
        std::vector<std::string> vec;
        QStringList fields = str.split(QRegularExpression(delimiterRegex), QString::SkipEmptyParts);
        vec.reserve(fields.size());
        foreach(QString field, fields) {
            field = field.trimmed();
            if(!field.isEmpty()) {
                vec.push_back(field.toStdString());
            }
        }
        return vec;
    }
    
#ifdef DEBUG
#define VERIFY(x, errMsg) if(!(x)) { ++numErrors; std::cout << errMsg << std::endl; }
    void test()
    {
        int numErrors = 0;
        
        StimulusClampProtocol protocol;
        
        protocol.dump(std::cout);
        
        std::cout << "Test completed with " << numErrors << " error(s)." << std::endl;
    }
#endif

} // StimulusClampProtocol
