#!/usr/bin/env bash
# Wrapper to invoke libiec61850 genmodel.jar against target .icd/.scd files.
#
# Not used today: the "Reporter1" model in src/sim_server.c is hand-authored
# directly via the dynamic model API (IedModel_create/LogicalDevice_create/...)
# rather than generated, since genmodel.jar's output is verbose/heavyweight
# for the tiny single-LD fixture the E2E tests need. genmodel.jar itself is
# available in the sibling libiec61850 checkout
# (../../../libiec61850/tools/model_generator/genmodel.jar, matching the
# mxml/Unity sibling-checkout vendoring convention in CLAUDE.md) if a future,
# larger simulated device ever warrants real codegen - invoke it as:
#   java -jar <path-to>/genmodel.jar <file.icd> -out models/<name>
echo "[SIM] Model generation script placeholder - see comments in this file."
