/**
 Copyright (c) 2015-present, Facebook, Inc.
 All rights reserved.

 This source code is licensed under the BSD-style license found in the
 LICENSE file in the root directory of this source tree. An additional grant
 of patent rights can be found in the PATENTS file in the same directory.
 */

#include <xcexecution/SimpleExecutor.h>

#include <builtin/Driver.h>
#include <pbxbuild/Phase/Environment.h>
#include <pbxbuild/Phase/PhaseInvocations.h>

#include <fstream>

#include <sys/types.h>
#include <sys/stat.h>

using xcexecution::SimpleExecutor;
using libutil::FSUtil;
using libutil::Subprocess;

SimpleExecutor::
SimpleExecutor(std::shared_ptr<Formatter> const &formatter, bool dryRun, builtin::Registry const &builtins) :
    Executor (formatter, dryRun),
    _builtins(builtins)
{
}

SimpleExecutor::
~SimpleExecutor()
{
}

bool SimpleExecutor::
build(
    pbxbuild::Build::Environment const &buildEnvironment,
    pbxbuild::Build::Context const &buildContext,
    pbxbuild::DirectedGraph<pbxproj::PBX::Target::shared_ptr> const &targetGraph)
{
    Formatter::Print(_formatter->begin(buildContext));

    ext::optional<std::vector<pbxproj::PBX::Target::shared_ptr>> orderedTargets = targetGraph.ordered();
    if (!orderedTargets) {
        fprintf(stderr, "error: cycle detected in target dependencies\n");
        return false;
    }

    for (pbxproj::PBX::Target::shared_ptr const &target : *orderedTargets) {
        Formatter::Print(_formatter->beginTarget(buildContext, target));

        ext::optional<pbxbuild::Target::Environment> targetEnvironment = buildContext.targetEnvironment(buildEnvironment, target);
        if (!targetEnvironment) {
            fprintf(stderr, "error: couldn't create target environment for %s\n", target->name().c_str());
            Formatter::Print(_formatter->finishTarget(buildContext, target));
            continue;
        }

        Formatter::Print(_formatter->beginCheckDependencies(target));
        pbxbuild::Phase::Environment phaseEnvironment = pbxbuild::Phase::Environment(buildEnvironment, buildContext, target, *targetEnvironment);
        pbxbuild::Phase::PhaseInvocations phaseInvocations = pbxbuild::Phase::PhaseInvocations::Create(phaseEnvironment, target);
        Formatter::Print(_formatter->finishCheckDependencies(target));

        auto result = buildTarget(target, *targetEnvironment, phaseInvocations.invocations());
        if (!result.first) {
            Formatter::Print(_formatter->finishTarget(buildContext, target));
            Formatter::Print(_formatter->failure(buildContext, result.second));
            return false;
        }

        Formatter::Print(_formatter->finishTarget(buildContext, target));
    }

    Formatter::Print(_formatter->success(buildContext));
    return true;
}

static ext::optional<std::vector<pbxbuild::Tool::Invocation const>>
SortInvocations(std::vector<pbxbuild::Tool::Invocation const> const &invocations)
{
    std::unordered_map<std::string, pbxbuild::Tool::Invocation const *> outputToInvocation;
    for (pbxbuild::Tool::Invocation const &invocation : invocations) {
        for (std::string const &output : invocation.outputs()) {
            outputToInvocation.insert({ output, &invocation });
        }
    }

    pbxbuild::DirectedGraph<pbxbuild::Tool::Invocation const *> graph;
    for (pbxbuild::Tool::Invocation const &invocation : invocations) {
        graph.insert(&invocation, { });

        for (std::string const &input : invocation.inputs()) {
            auto it = outputToInvocation.find(input);
            if (it != outputToInvocation.end()) {
                graph.insert(&invocation, { it->second });
            }
        }
        for (std::string const &phonyInputs : invocation.phonyInputs()) {
            auto it = outputToInvocation.find(phonyInputs);
            if (it != outputToInvocation.end()) {
                graph.insert(&invocation, { it->second });
            }
        }
        for (std::string const &inputDependency : invocation.inputDependencies()) {
            auto it = outputToInvocation.find(inputDependency);
            if (it != outputToInvocation.end()) {
                graph.insert(&invocation, { it->second });
            }
        }
    }

    std::vector<pbxbuild::Tool::Invocation const> result;

    ext::optional<std::vector<pbxbuild::Tool::Invocation const *>> orderedInvocations = graph.ordered();
    if (!orderedInvocations) {
        return ext::nullopt;
    }

    for (pbxbuild::Tool::Invocation const *invocation : *orderedInvocations) {
        result.push_back(*invocation);
    }
    return result;
}

bool SimpleExecutor::
writeAuxiliaryFiles(
    pbxproj::PBX::Target::shared_ptr const &target,
    pbxbuild::Target::Environment const &targetEnvironment,
    std::vector<pbxbuild::Tool::Invocation const> const &invocations)
{
    Formatter::Print(_formatter->beginWriteAuxiliaryFiles(target));
    for (pbxbuild::Tool::Invocation const &invocation : invocations) {
        for (pbxbuild::Tool::Invocation::AuxiliaryFile const &auxiliaryFile : invocation.auxiliaryFiles()) {
            std::string directory = FSUtil::GetDirectoryName(auxiliaryFile.path());
            if (!FSUtil::TestForDirectory(directory)) {
                Formatter::Print(_formatter->createAuxiliaryDirectory(directory));

                if (!_dryRun) {
                    if (!FSUtil::CreateDirectory(directory)) {
                        return false;
                    }
                }
            }

            Formatter::Print(_formatter->writeAuxiliaryFile(auxiliaryFile.path()));

            if (!_dryRun) {
                std::ofstream out;
                out.open(auxiliaryFile.path(), std::ios::out | std::ios::trunc | std::ios::binary);
                if (out.fail()) {
                    return false;
                }

                std::copy(auxiliaryFile.contents().begin(), auxiliaryFile.contents().end(), std::ostream_iterator<char>(out));
                out.close();
            }

            if (auxiliaryFile.executable() && !FSUtil::TestForExecute(auxiliaryFile.path())) {
                Formatter::Print(_formatter->setAuxiliaryExecutable(auxiliaryFile.path()));

                if (!_dryRun) {
                    if (::chmod(auxiliaryFile.path().c_str(), S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) != 0) {
                        return false;
                    }
                }
            }
        }
    }
    Formatter::Print(_formatter->finishWriteAuxiliaryFiles(target));

    return true;
}

std::pair<bool, std::vector<pbxbuild::Tool::Invocation const>> SimpleExecutor::
performInvocations(
    pbxproj::PBX::Target::shared_ptr const &target,
    pbxbuild::Target::Environment const &targetEnvironment,
    std::vector<pbxbuild::Tool::Invocation const> const &orderedInvocations,
    bool createProductStructure)
{
    for (pbxbuild::Tool::Invocation const &invocation : orderedInvocations) {
        // TODO(grp): This should perhaps be a separate flag for a 'phony' invocation.
        if (invocation.executable().path().empty()) {
            continue;
        }

        if (invocation.createsProductStructure() != createProductStructure) {
            continue;
        }

        std::map<std::string, std::string> sortedEnvironment = std::map<std::string, std::string>(invocation.environment().begin(), invocation.environment().end());

        Formatter::Print(_formatter->beginInvocation(invocation, invocation.executable().displayName(), createProductStructure));

        if (!_dryRun) {
            for (std::string const &output : invocation.outputs()) {
                std::string directory = FSUtil::GetDirectoryName(output);

                if (!FSUtil::CreateDirectory(directory)) {
                    return std::make_pair(false, std::vector<pbxbuild::Tool::Invocation const>({ invocation }));
                }
            }

            if (!invocation.executable().builtin().empty()) {
                /* For built-in tools, run them in-process. */
                std::shared_ptr<builtin::Driver> driver = _builtins.driver(invocation.executable().builtin());
                if (driver == nullptr) {
                    Formatter::Print(_formatter->finishInvocation(invocation, invocation.executable().displayName(), createProductStructure));
                    return std::make_pair(false, std::vector<pbxbuild::Tool::Invocation const>({ invocation }));
                }

                if (driver->run(invocation.arguments(), invocation.environment(), invocation.workingDirectory()) != 0) {
                    Formatter::Print(_formatter->finishInvocation(invocation, invocation.executable().displayName(), createProductStructure));
                    return std::make_pair(false, std::vector<pbxbuild::Tool::Invocation const>({ invocation }));
                }
            } else {
                /* External tool, run the tool externally. */
                Subprocess process;
                if (!process.execute(invocation.executable().path(), invocation.arguments(), invocation.environment(), invocation.workingDirectory()) || process.exitcode() != 0) {
                    Formatter::Print(_formatter->finishInvocation(invocation, invocation.executable().displayName(), createProductStructure));
                    return std::make_pair(false, std::vector<pbxbuild::Tool::Invocation const>({ invocation }));
                }
            }
        }

        Formatter::Print(_formatter->finishInvocation(invocation, invocation.executable().displayName(), createProductStructure));
    }

    return std::make_pair(true, std::vector<pbxbuild::Tool::Invocation const>());
}

std::pair<bool, std::vector<pbxbuild::Tool::Invocation const>> SimpleExecutor::
buildTarget(
    pbxproj::PBX::Target::shared_ptr const &target,
    pbxbuild::Target::Environment const &targetEnvironment,
    std::vector<pbxbuild::Tool::Invocation const> const &invocations)
{
    if (!writeAuxiliaryFiles(target, targetEnvironment, invocations)) {
        return std::make_pair(false, std::vector<pbxbuild::Tool::Invocation const>());
    }

    ext::optional<std::vector<pbxbuild::Tool::Invocation const>> orderedInvocations = SortInvocations(invocations);
    if (!orderedInvocations) {
        fprintf(stderr, "error: cycle detected building invocation graph\n");
        return std::make_pair(false, std::vector<pbxbuild::Tool::Invocation const>());
    }

    Formatter::Print(_formatter->beginCreateProductStructure(target));
    std::pair<bool, std::vector<pbxbuild::Tool::Invocation const>> structureResult = performInvocations(target, targetEnvironment, *orderedInvocations, true);
    Formatter::Print(_formatter->finishCreateProductStructure(target));
    if (!structureResult.first) {
        return structureResult;
    }

    std::pair<bool, std::vector<pbxbuild::Tool::Invocation const>> invocationsResult = performInvocations(target, targetEnvironment, *orderedInvocations, false);
    if (!invocationsResult.first) {
        return invocationsResult;
    }

    return std::make_pair(true, std::vector<pbxbuild::Tool::Invocation const>());
}

std::unique_ptr<SimpleExecutor> SimpleExecutor::
Create(std::shared_ptr<Formatter> const &formatter, bool dryRun, builtin::Registry const &builtins)
{
    return std::unique_ptr<SimpleExecutor>(new SimpleExecutor(
        formatter,
        dryRun,
        builtins
    ));
}