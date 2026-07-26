#!/usr/bin/env bash

# Exit immediately if a command exits with a non-zero status
set -e

SIM_DIR="integration_tests/ied_simulator"

echo "[CORE] Scaffolding IED Simulator test environment..."

# Create isolated subdirectories
mkdir -p "${SIM_DIR}/src"
mkdir -p "${SIM_DIR}/models"
mkdir -p "${SIM_DIR}/scripts"

# Scaffold required implementation and configuration files
touch "${SIM_DIR}/Makefile"
touch "${SIM_DIR}/src/sim_server.c"
touch "${SIM_DIR}/src/sim_types.h"

# Scaffold model generation wrapper script
cat << 'EOF' > "${SIM_DIR}/scripts/generate_model.sh"
#!/usr/bin/env bash
# Wrapper to invoke libiec61850 genmodel.jar against target .icd/.scd files
echo "[SIM] Model generation script placeholder."
EOF

# Make the utility script executable
chmod +x "${SIM_DIR}/scripts/generate_model.sh"

echo "=========================================================================="
echo "[SUCCESS] Simulator tree created safely outside of production source path."
echo "Location: ./${SIM_DIR}"
echo "=========================================================================="