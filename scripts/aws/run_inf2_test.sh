#!/bin/bash
# uracil/scripts/aws/run_inf2_test.sh
#
# Build the pytorch-neuron-service on an Amazon Inferentia 2 (inf2.xlarge)
# instance and run it to verify NPU acceleration via torch_neuronx.
#
# Reuses an existing instance (by Name tag) or launches a fresh inf2.xlarge.
# All remote execution is via SSM — no open SSH ingress required.
#
# Environment variables:
#   AWS_REGION                  default: us-east-2
#   REUSE_INSTANCE_NAME         Name tag to prefer for reuse
#                               (default: uracil-neuron-test;
#                                try: athena-gguf-neuron-sidecar)
#   INSTANCE_TYPE               default: inf2.xlarge
#   ATHENA_VPC_NAME             default: keryx-vpc
#   ATHENA_PUBLIC_SUBNET_NAME   default: keryx-public-a
#   URA_NEURON_IAM_ROLE         IAM role for SSM  (reuses athena's by default)
#   URA_NEURON_IAM_PROFILE      Instance profile  (reuses athena's by default)
#   UBUNTU_RELEASE              default: 24.04
#   TERMINATE_AFTER             set to 1 to terminate after test
#
# Output:
#   Prints structured JSON from each test function (health, infer).
#   If neuron_available == true in the infer output, NPU is active.

set -euo pipefail

AWS_REGION="${AWS_REGION:-us-east-2}"
REUSE_INSTANCE_NAME="${REUSE_INSTANCE_NAME:-uracil-neuron-test}"
INSTANCE_TYPE="${INSTANCE_TYPE:-inf2.xlarge}"
ATHENA_VPC_NAME="${ATHENA_VPC_NAME:-keryx-vpc}"
ATHENA_PUBLIC_SUBNET_NAME="${ATHENA_PUBLIC_SUBNET_NAME:-keryx-public-a}"
IAM_ROLE_NAME="${URA_NEURON_IAM_ROLE:-athena-neuron-sidecar-ssm-role}"
IAM_PROFILE_NAME="${URA_NEURON_IAM_PROFILE:-athena-neuron-sidecar-ssm-profile}"
UBUNTU_RELEASE="${UBUNTU_RELEASE:-24.04}"
TERMINATE_AFTER="${TERMINATE_AFTER:-0}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
URACIL_DIR="$ROOT_DIR/uracil"

# ── Helpers ──────────────────────────────────────────────────────────────

log() { printf '[ura-inf2-test] %s\n' "$1" >&2; }

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1" >&2; exit 1
  fi
}

need_cmd aws
need_cmd jq
need_cmd base64
need_cmd gzip
need_cmd tar

aws sts get-caller-identity >/dev/null

# ── IAM: ensure SSM instance profile exists (reuse athena's) ─────────────

ensure_ssm_profile() {
  if ! aws iam get-role --role-name "$IAM_ROLE_NAME" >/dev/null 2>&1; then
    log "Creating IAM role $IAM_ROLE_NAME ..."
    aws iam create-role \
      --role-name "$IAM_ROLE_NAME" \
      --assume-role-policy-document '{"Version":"2012-10-17","Statement":[{"Effect":"Allow","Principal":{"Service":"ec2.amazonaws.com"},"Action":"sts:AssumeRole"}]}' >/dev/null
  fi
  aws iam attach-role-policy \
    --role-name "$IAM_ROLE_NAME" \
    --policy-arn arn:aws:iam::aws:policy/AmazonSSMManagedInstanceCore >/dev/null 2>&1 || true

  if ! aws iam get-instance-profile --instance-profile-name "$IAM_PROFILE_NAME" >/dev/null 2>&1; then
    log "Creating instance profile $IAM_PROFILE_NAME ..."
    aws iam create-instance-profile --instance-profile-name "$IAM_PROFILE_NAME" >/dev/null
  fi

  local roles
  roles=$(aws iam get-instance-profile \
    --instance-profile-name "$IAM_PROFILE_NAME" \
    --query 'InstanceProfile.Roles[].RoleName' --output text 2>/dev/null || true)
  if ! grep -qw "$IAM_ROLE_NAME" <<<"$roles"; then
    aws iam add-role-to-instance-profile \
      --instance-profile-name "$IAM_PROFILE_NAME" \
      --role-name "$IAM_ROLE_NAME" >/dev/null
  fi
}

associate_profile() {
  local iid="$1"
  local cur
  cur=$(aws ec2 describe-instances \
    --instance-ids "$iid" --region "$AWS_REGION" \
    --query 'Reservations[0].Instances[0].IamInstanceProfile.Arn' \
    --output text 2>/dev/null || true)
  [[ -n "$cur" && "$cur" != "None" ]] && return

  local assoc
  assoc=$(aws ec2 describe-iam-instance-profile-associations \
    --filters "Name=instance-id,Values=${iid}" --region "$AWS_REGION" \
    --query 'IamInstanceProfileAssociations[0].AssociationId' \
    --output text 2>/dev/null || true)
  if [[ -z "$assoc" || "$assoc" == "None" ]]; then
    aws ec2 associate-iam-instance-profile \
      --instance-id "$iid" \
      --iam-instance-profile "Name=${IAM_PROFILE_NAME}" \
      --region "$AWS_REGION" >/dev/null
  fi
}

ensure_ssm_profile

# ── Find or launch inf2 instance ─────────────────────────────────────────

# Search order: REUSE_INSTANCE_NAME tag first, then athena sidecar name.
find_instance() {
  local name="$1"
  aws ec2 describe-instances \
    --filters \
      "Name=tag:Name,Values=${name}" \
      "Name=instance-state-name,Values=running,pending" \
    --query "Reservations[0].Instances[0].InstanceId" \
    --output text --region "$AWS_REGION" 2>/dev/null || true
}

INSTANCE_ID=""
for try_name in "$REUSE_INSTANCE_NAME" "athena-gguf-neuron-sidecar"; do
  candidate=$(find_instance "$try_name")
  if [[ -n "$candidate" && "$candidate" != "None" ]]; then
    INSTANCE_ID="$candidate"
    log "Reusing existing instance $INSTANCE_ID (Name=${try_name})"
    associate_profile "$INSTANCE_ID"
    break
  fi
done

if [[ -z "$INSTANCE_ID" ]]; then
  log "No existing inf2 instance found — launching $INSTANCE_TYPE ..."

  ubuntu_codename="noble"
  [[ "${UBUNTU_RELEASE%%.*}" == "22" ]] && ubuntu_codename="jammy"

  AMI_ID=$(aws ssm get-parameter \
    --name "/aws/service/canonical/ubuntu/server/${UBUNTU_RELEASE}/stable/current/amd64/hvm/ebs-gp3/ami-id" \
    --region "$AWS_REGION" --query 'Parameter.Value' --output text 2>/dev/null || true)

  if [[ -z "$AMI_ID" || "$AMI_ID" == "None" ]]; then
    AMI_ID=$(aws ec2 describe-images \
      --owners 099720109477 \
      --filters "Name=name,Values=ubuntu/images/hvm-ssd/ubuntu-${ubuntu_codename}-${UBUNTU_RELEASE}-amd64-server-*" \
      --query 'sort_by(Images, &CreationDate)[-1].ImageId' \
      --region "$AWS_REGION" --output text)
  fi

  VPC_ID=$(aws ec2 describe-vpcs \
    --filters "Name=tag:Name,Values=${ATHENA_VPC_NAME}" \
    --query "Vpcs[0].VpcId" --output text --region "$AWS_REGION")
  [[ -z "$VPC_ID" || "$VPC_ID" == "None" ]] && { echo "VPC ${ATHENA_VPC_NAME} not found" >&2; exit 1; }

  SUBNET_ID=$(aws ec2 describe-subnets \
    --filters "Name=tag:Name,Values=${ATHENA_PUBLIC_SUBNET_NAME}" "Name=vpc-id,Values=${VPC_ID}" \
    --query "Subnets[0].SubnetId" --output text --region "$AWS_REGION")
  [[ -z "$SUBNET_ID" || "$SUBNET_ID" == "None" ]] && { echo "Subnet ${ATHENA_PUBLIC_SUBNET_NAME} not found" >&2; exit 1; }

  # Minimal security group: no inbound (SSM is outbound-only)
  SG_ID=$(aws ec2 describe-security-groups \
    --filters "Name=group-name,Values=uracil-neuron-test-sg" "Name=vpc-id,Values=${VPC_ID}" \
    --query "SecurityGroups[0].GroupId" --output text --region "$AWS_REGION" 2>/dev/null || true)
  if [[ -z "$SG_ID" || "$SG_ID" == "None" ]]; then
    SG_ID=$(aws ec2 create-security-group \
      --group-name "uracil-neuron-test-sg" \
      --description "Uracil Neuron test — no inbound (SSM only)" \
      --vpc-id "$VPC_ID" --query "GroupId" --output text --region "$AWS_REGION")
  fi

  # User-data: install SSM agent + Neuron kernel driver
  UBUNTU_SERIES="${UBUNTU_RELEASE%%.*}"
  user_data=$(cat <<EOF
#!/bin/bash
set -euo pipefail
exec > /var/log/uracil-neuron-init.log 2>&1

apt-get update -y
apt-get install -y snapd curl wget
systemctl enable --now snapd.socket || true
snap install amazon-ssm-agent --classic || true
systemctl enable snap.amazon-ssm-agent.amazon-ssm-agent.service || true
systemctl start  snap.amazon-ssm-agent.amazon-ssm-agent.service || true

. /etc/os-release
echo "deb https://apt.repos.neuron.amazonaws.com \$(lsb_release -sc) main" > /etc/apt/sources.list.d/neuron.list
wget -qO - https://apt.repos.neuron.amazonaws.com/GPG-PUB-KEY-AMAZON-AWS-NEURON.PUB | apt-key add -
apt-get update -y
apt-get install -y aws-neuronx-dkms || true
systemctl restart neuron-monitor || true
EOF
)

  INSTANCE_ID=$(aws ec2 run-instances \
    --image-id "$AMI_ID" \
    --instance-type "$INSTANCE_TYPE" \
    --subnet-id "$SUBNET_ID" \
    --security-group-ids "$SG_ID" \
    --iam-instance-profile "Name=${IAM_PROFILE_NAME}" \
    --associate-public-ip-address \
    --user-data "$user_data" \
    --block-device-mappings '[{"DeviceName":"/dev/sda1","Ebs":{"VolumeSize":60,"VolumeType":"gp3"}}]' \
    --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=${REUSE_INSTANCE_NAME}},{Key=Project,Value=uracil}]" \
    --region "$AWS_REGION" \
    --query "Instances[0].InstanceId" \
    --output text)

  log "Launched $INSTANCE_ID — waiting for running state ..."
  aws ec2 wait instance-running --instance-ids "$INSTANCE_ID" --region "$AWS_REGION"
  log "Instance running. Waiting 60 s for user-data + SSM agent to complete ..."
  sleep 60
fi

# ── Wait for SSM connectivity ─────────────────────────────────────────────

log "Waiting for SSM agent on $INSTANCE_ID ..."
for i in $(seq 1 30); do
  ssm_state=$(aws ssm describe-instance-information \
    --filters "Key=InstanceIds,Values=${INSTANCE_ID}" \
    --region "$AWS_REGION" \
    --query 'InstanceInformationList[0].PingStatus' \
    --output text 2>/dev/null || true)
  if [[ "$ssm_state" == "Online" ]]; then
    log "SSM agent online."
    break
  fi
  [[ $i -eq 30 ]] && { echo "Timed out waiting for SSM agent" >&2; exit 1; }
  sleep 10
done

# ── Pack uracil source as gzipped archive ─────────────────────────────────

log "Packing uracil source ..."
ARCHIVE_B64=$(cd "$URACIL_DIR" && \
  tar czf - \
    stub/ura_stub.c \
    tools/ura_builder.py \
    tools/neuron_torch_shim.py \
    examples/pytorch-neuron-service/manifest.json \
    examples/pytorch-neuron-service/functions/health.py \
    examples/pytorch-neuron-service/functions/infer.py \
    examples/pytorch-neuron-service/build_aps.py \
  | base64 -w 0)

log "Archive: $(echo "$ARCHIVE_B64" | wc -c) bytes base64"

# ── Build the remote test script ──────────────────────────────────────────
#
# ${ARCHIVE_B64} expands on this machine (safe — no secrets, just source).
# All \$ are escaped so they evaluate on the remote host.
#
# torch-neuronx requires a matching torch version from the Neuron pip repo:
#   https://pip.repos.neuron.amazonaws.com
# The install also brings in neuronx-cc (the Neuron compiler).
# First install takes ~5-10 min; results are cached in the venv.

remote_script=$(cat <<REMOTE_EOF
#!/bin/bash
set -euo pipefail
# Tee all output to the log file AND stdout (so SSM StandardOutputContent is populated).
exec > >(tee -a /var/log/uracil-neuron-test.log) 2>&1
echo "=== uracil-neuron-test started at \$(date) ==="

WORKDIR=/opt/uracil-neuron-test
mkdir -p "\$WORKDIR"
cd "\$WORKDIR"

# ── System packages ───────────────────────────────────────────────────────
apt-get update -q
apt-get install -y -q gcc python3 python3-pip python3-venv

# ── Install Neuron APT packages (idempotent) ─────────────────────────────
# aws-neuronx-dkms           — kernel module (hardware access)
# aws-neuronx-runtime-lib    — libnrt.so.1 (required by torch_neuronx at import)
# aws-neuronx-collectives    — libnccom.so (required for multi-NeuronCore NCCL ops)
if ! dpkg -l aws-neuronx-collectives >/dev/null 2>&1; then
  . /etc/os-release
  echo "deb https://apt.repos.neuron.amazonaws.com \$(lsb_release -sc) main" \
    > /etc/apt/sources.list.d/neuron.list
  wget -qO - https://apt.repos.neuron.amazonaws.com/GPG-PUB-KEY-AMAZON-AWS-NEURON.PUB | apt-key add -
  apt-get update -q
  apt-get install -y -q aws-neuronx-dkms aws-neuronx-runtime-lib aws-neuronx-collectives || echo "WARN: Neuron packages install failed"
fi

# ── Python venv + torch-neuronx ───────────────────────────────────────────
VENV="\$WORKDIR/.venv"
if [[ ! -x "\$VENV/bin/python3" ]]; then
  python3 -m venv "\$VENV"
fi

PIP="\$VENV/bin/pip"
"\$PIP" install --quiet --upgrade pip

# Install torch + torch-neuronx from the Neuron pip index.
# This brings in a torch version that matches the Neuron compiler.
if ! "\$VENV/bin/python3" -c "import torch_neuronx" 2>/dev/null; then
  echo "Installing torch-neuronx (this may take several minutes) ..."
  "\$PIP" install --quiet \
    torch-neuronx \
    neuronx-cc \
    --extra-index-url https://pip.repos.neuron.amazonaws.com
  # Verify import succeeds (OSError means missing libnrt.so.1, not a pip problem)
  "\$VENV/bin/python3" -c "import torch_neuronx" 2>&1 | head -5 || echo "WARN: torch_neuronx import check failed"
fi

# ── Extract uracil source ─────────────────────────────────────────────────
echo '${ARCHIVE_B64}' | base64 -d | tar xzf - --overwrite

# ── Build the .ura binary ─────────────────────────────────────────────────
mkdir -p "\$WORKDIR/examples/pytorch-neuron-service/dist"
"\$VENV/bin/python3" "\$WORKDIR/examples/pytorch-neuron-service/build_aps.py"

# ── Run tests via the .ura binary ─────────────────────────────────────────
# Put the venv python first so find_python() inside the stub picks it up.
export PATH="\$VENV/bin:\$PATH"
# NEURON_RT_NUM_CORES=1 — use a single NeuronCore; avoids collective-comm
# (NCCL/libnccom) initialisation which is only needed for multi-core models.
export NEURON_RT_NUM_CORES=1
BINARY="\$WORKDIR/examples/pytorch-neuron-service/dist/pytorch-neuron-service.ura"

echo "=== --describe ==="
"\$BINARY" --describe

echo "=== health ==="
"\$BINARY" health

echo "=== infer ==="
"\$BINARY" infer

echo "=== test complete at \$(date) ==="
REMOTE_EOF
)

REMOTE_SCRIPT_B64=$(printf '%s' "$remote_script" | base64 -w 0)

# ── Send SSM command ──────────────────────────────────────────────────────

log "Sending test command via SSM ..."
PARAMS_FILE=$(mktemp)
jq -n --arg cmd "echo '${REMOTE_SCRIPT_B64}' | base64 -d | bash" \
  '{"commands": [$cmd]}' > "$PARAMS_FILE"

CMD_ID=$(aws ssm send-command \
  --instance-ids "$INSTANCE_ID" \
  --document-name "AWS-RunShellScript" \
  --region "$AWS_REGION" \
  --timeout-seconds 1800 \
  --parameters "file://${PARAMS_FILE}" \
  --output text \
  --query 'Command.CommandId')
rm -f "$PARAMS_FILE"

log "SSM command $CMD_ID — polling (up to 30 min for first torch-neuronx install) ..."

# ── Poll for completion ───────────────────────────────────────────────────

SUCCESS=0
for i in $(seq 1 180); do
  STATUS=$(aws ssm get-command-invocation \
    --command-id "$CMD_ID" \
    --instance-id "$INSTANCE_ID" \
    --region "$AWS_REGION" \
    --query 'StatusDetails' \
    --output text 2>/dev/null || echo "Pending")
  case "$STATUS" in
    Success)
      SUCCESS=1
      log "Command completed successfully."
      break
      ;;
    Failed|TimedOut|Cancelled|Undeliverable|Terminated)
      log "Command failed with status: $STATUS"
      aws ssm get-command-invocation \
        --command-id "$CMD_ID" \
        --instance-id "$INSTANCE_ID" \
        --region "$AWS_REGION" \
        --query 'StandardErrorContent' \
        --output text >&2 || true
      break
      ;;
  esac
  sleep 10
done

# ── Print output ──────────────────────────────────────────────────────────

STDOUT=$(aws ssm get-command-invocation \
  --command-id "$CMD_ID" \
  --instance-id "$INSTANCE_ID" \
  --region "$AWS_REGION" \
  --query 'StandardOutputContent' \
  --output text 2>/dev/null || true)

STDERR=$(aws ssm get-command-invocation \
  --command-id "$CMD_ID" \
  --instance-id "$INSTANCE_ID" \
  --region "$AWS_REGION" \
  --query 'StandardErrorContent' \
  --output text 2>/dev/null || true)

echo ""
echo "══════════════════════════════════════════"
echo " REMOTE STDOUT"
echo "══════════════════════════════════════════"
echo "$STDOUT"

if [[ -n "$STDERR" ]]; then
  echo ""
  echo "══════════════════════════════════════════"
  echo " REMOTE STDERR (shim + routing logs)"
  echo "══════════════════════════════════════════"
  echo "$STDERR"
fi

# ── NPU acceleration verdict ──────────────────────────────────────────────

echo ""
echo "══════════════════════════════════════════"
echo " VERDICT"
echo "══════════════════════════════════════════"

NEURON_AVAILABLE=$(echo "$STDOUT" | grep '"neuron_available"' | tail -1 | grep -o 'true\|false' || echo "parse-failed")

if [[ "$NEURON_AVAILABLE" == "True" || "$NEURON_AVAILABLE" == "true" ]]; then
  echo " NPU ACCELERATED — torch_neuronx is active, model traced via Neuron compiler"
elif [[ "$NEURON_AVAILABLE" == "False" || "$NEURON_AVAILABLE" == "false" ]]; then
  echo " NOT NPU ACCELERATED — torch_neuronx was not available; ran on CPU fallback"
  echo " Check: was aws-neuronx-dkms loaded? Does the AMI support Inf2?"
else
  echo " UNABLE TO DETERMINE (neuron_available=$NEURON_AVAILABLE)"
  [[ $SUCCESS -eq 0 ]] && echo " Command did not complete successfully — see STDERR above"
fi

echo ""

# ── Optional teardown ─────────────────────────────────────────────────────

if [[ "$TERMINATE_AFTER" == "1" ]]; then
  log "TERMINATE_AFTER=1 — terminating instance $INSTANCE_ID ..."
  aws ec2 terminate-instances --instance-ids "$INSTANCE_ID" --region "$AWS_REGION" >/dev/null
  log "Termination requested."
else
  log "Instance $INSTANCE_ID left running. Set TERMINATE_AFTER=1 to auto-terminate."
fi

[[ $SUCCESS -eq 1 ]] && exit 0 || exit 1
