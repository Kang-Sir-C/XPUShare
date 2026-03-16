#!/usr/bin/env bash
set -u

# Strict E4 runner:
# - ensures A/B both Succeeded before parsing
# - checks logs contain a final throughput line (iter_per_sec=...)
# - records nodeName + sharedgpu/gpu_uuid for co-location sanity
# - only summarizes valid runs
#
# Usage (run from anywhere):
#   cd ~/XPUShare/exp
#   chmod +x ./run_e4_batch_strict.sh
#   ./run_e4_batch_strict.sh 10
#
# Override (optional):
#   SCHED=xpushare-scheduler OUT_DIR=~/XPUShare/exp/e4年后第三次实验结果 ./run_e4_batch_strict.sh 10
#   REPORT_EVERY=1 ./run_e4_batch_strict.sh 10   # reduces tail-drift by synchronizing periodically
#

N_RUNS="${1:-10}"
SCHED="${SCHED:-xpushare-scheduler}"
OUT_DIR="${OUT_DIR:-}"
# Default to 1s synchronized reporting to (1) measure effective throughput
# instead of enqueue rate, and (2) reduce phase-alignment variance against
# time-slice enforcement.
REPORT_EVERY="${REPORT_EVERY:-1}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
YAML_TEMPLATE="${YAML_TEMPLATE:-${SCRIPT_DIR}/e4-fairness.template.yaml}"
PY_PARSE="${PY_PARSE:-${SCRIPT_DIR}/e4_parse_fairness.py}"
PY_SUM="${PY_SUM:-${SCRIPT_DIR}/e4_summarize_fairness.py}"

if [[ -z "${OUT_DIR}" ]]; then
  OUT_DIR="${SCRIPT_DIR}/e4年后第三次实验结果/e4-batch-$(date +%Y%m%d-%H%M%S)"
fi
mkdir -p "${OUT_DIR}"

ts() { date +"%F %T"; }

die() {
  echo "[$(ts)][FATAL] $*" >&2
  exit 2
}

need() {
  command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

need kubectl
need python3
need sed
need grep
need awk

[[ -f "${YAML_TEMPLATE}" ]] || die "YAML_TEMPLATE not found: ${YAML_TEMPLATE}"
[[ -f "${PY_PARSE}" ]] || die "parser not found: ${PY_PARSE}"
[[ -f "${PY_SUM}" ]] || die "summarizer not found: ${PY_SUM}"

echo "[$(ts)] E4 batch start N_RUNS=${N_RUNS} SCHED=${SCHED} OUT_DIR=${OUT_DIR} REPORT_EVERY=${REPORT_EVERY}"
echo "[$(ts)]   YAML_TEMPLATE=${YAML_TEMPLATE}"
echo "[$(ts)]   PY_PARSE=${PY_PARSE}"
echo "[$(ts)]   PY_SUM=${PY_SUM}"

valid_csvs=()
invalid_runs=()

apply_yaml() {
  # Replace schedulerName lines and optionally add --report-every to the command.
  if [[ "${REPORT_EVERY}" != "0" ]]; then
    sed -E \
      -e "s/^([[:space:]]*schedulerName:).*/\\1 ${SCHED}/" \
      -e "s/(python3 \\/work\\/e4_microbench_torch\\.py[^\\n\"]*)\"/\\1 --report-every ${REPORT_EVERY}\"/g" \
      "${YAML_TEMPLATE}" | kubectl apply -f -
  else
    sed -E \
      -e "s/^([[:space:]]*schedulerName:).*/\\1 ${SCHED}/" \
      "${YAML_TEMPLATE}" | kubectl apply -f -
  fi
}

wait_deleted() {
  kubectl delete pod xpushare-e4-a xpushare-e4-b --ignore-not-found --wait=true >/dev/null 2>&1 || true
  # ConfigMap name is fixed in the template; keep it clean between runs.
  kubectl delete configmap xpushare-e4-script --ignore-not-found >/dev/null 2>&1 || true
}

phase() {
  kubectl get pod "$1" -o jsonpath='{.status.phase}' 2>/dev/null || echo ""
}

node_and_uuid() {
  local pod="$1"
  kubectl get pod "${pod}" -o jsonpath='{.spec.nodeName}{"\t"}{.metadata.annotations.sharedgpu\/gpu_uuid}{"\n"}' 2>/dev/null || true
}

wait_exists() {
  local pod="$1" timeout_sec="${2:-60}"
  local t0 now
  t0="$(date +%s)"
  while true; do
    if kubectl get pod "${pod}" >/dev/null 2>&1; then
      return 0
    fi
    now="$(date +%s)"
    if (( now - t0 > timeout_sec )); then
      return 1
    fi
    sleep 1
  done
}

wait_uuid_ready() {
  local pod="$1" timeout_sec="${2:-60}"
  local t0 now uuid
  t0="$(date +%s)"
  while true; do
    uuid="$(kubectl get pod "${pod}" -o jsonpath='{.metadata.annotations.sharedgpu\/gpu_uuid}' 2>/dev/null || true)"
    if [[ -n "${uuid}" ]]; then
      return 0
    fi
    now="$(date +%s)"
    if (( now - t0 > timeout_sec )); then
      return 1
    fi
    sleep 1
  done
}

dump_describe() {
  local pod="$1" out="$2"
  kubectl describe pod "${pod}" > "${out}" 2>&1 || true
}

has_result_line() {
  local f="$1"
  grep -Eq "\\[E4\\] (result|done) .*iter_per_sec=" "${f}"
}

for i in $(seq -w 1 "${N_RUNS}"); do
  echo "[$(ts)] >>> run=${i} cleanup"
  wait_deleted

  echo "[$(ts)] >>> run=${i} apply yaml (scheduler=${SCHED})"
  if ! apply_yaml >"${OUT_DIR}/e4-${i}.apply.txt" 2>&1; then
    echo "[$(ts)][WARN] run=${i} kubectl apply failed; see ${OUT_DIR}/e4-${i}.apply.txt"
    invalid_runs+=("${i}:apply_failed")
    continue
  fi

  if ! wait_exists xpushare-e4-a 60 || ! wait_exists xpushare-e4-b 60; then
    echo "[$(ts)][WARN] run=${i} pods not created in time"
    invalid_runs+=("${i}:pods_not_created")
    continue
  fi

  echo "[$(ts)] >>> run=${i} wait scheduled"
  kubectl wait --for=condition=PodScheduled pod/xpushare-e4-a --timeout=120s >/dev/null 2>&1 || true
  kubectl wait --for=condition=PodScheduled pod/xpushare-e4-b --timeout=120s >/dev/null 2>&1 || true

  # Wait until scheduler writes annotations (uuid). Without this, co-location
  # cannot be validated and the run is not paper-usable.
  wait_uuid_ready xpushare-e4-a 60 || true
  wait_uuid_ready xpushare-e4-b 60 || true

  echo "[$(ts)] >>> run=${i} bind snapshot (node + gpu_uuid)"
  node_and_uuid xpushare-e4-a | tee "${OUT_DIR}/e4-${i}.a.bind.txt" >/dev/null
  node_and_uuid xpushare-e4-b | tee "${OUT_DIR}/e4-${i}.b.bind.txt" >/dev/null

  a_bind="$(cat "${OUT_DIR}/e4-${i}.a.bind.txt" 2>/dev/null || true)"
  b_bind="$(cat "${OUT_DIR}/e4-${i}.b.bind.txt" 2>/dev/null || true)"
  a_node="$(echo "${a_bind}" | awk '{print $1}')"
  b_node="$(echo "${b_bind}" | awk '{print $1}')"
  a_uuid="$(echo "${a_bind}" | awk '{print $2}')"
  b_uuid="$(echo "${b_bind}" | awk '{print $2}')"

  colocated_ok=1
  if [[ -z "${a_node}" || -z "${b_node}" || -z "${a_uuid}" || -z "${b_uuid}" ]]; then
    echo "[$(ts)][WARN] run=${i} missing node/uuid annotation (a='${a_bind}' b='${b_bind}')"
    invalid_runs+=("${i}:missing_uuid")
    colocated_ok=0
  elif [[ "${a_node}" != "${b_node}" || "${a_uuid}" != "${b_uuid}" ]]; then
    echo "[$(ts)][WARN] run=${i} not co-located (a='${a_bind}' b='${b_bind}')"
    invalid_runs+=("${i}:not_colocated")
    colocated_ok=0
  fi

  if [[ "${colocated_ok}" == "0" ]]; then
    dump_describe xpushare-e4-a "${OUT_DIR}/e4-${i}.a.describe.txt"
    dump_describe xpushare-e4-b "${OUT_DIR}/e4-${i}.b.describe.txt"
    kubectl delete pod xpushare-e4-a xpushare-e4-b --ignore-not-found --wait=true >/dev/null 2>&1 || true
    continue
  fi

  echo "[$(ts)] >>> run=${i} wait completion"
  t0="$(date +%s)"
  timeout_sec=600
  while true; do
    pa="$(phase xpushare-e4-a)"
    pb="$(phase xpushare-e4-b)"
    if [[ "${pa}" == "Succeeded" && "${pb}" == "Succeeded" ]]; then
      break
    fi
    if [[ "${pa}" == "Failed" || "${pb}" == "Failed" ]]; then
      echo "[$(ts)][WARN] run=${i} failed: phaseA=${pa} phaseB=${pb}"
      break
    fi
    now="$(date +%s)"
    if (( now - t0 > timeout_sec )); then
      echo "[$(ts)][WARN] run=${i} timeout waiting completion: phaseA=${pa} phaseB=${pb}"
      break
    fi
    sleep 5
  done

  echo "[$(ts)] >>> run=${i} collect logs"
  kubectl logs pod/xpushare-e4-a > "${OUT_DIR}/e4-${i}-a.log" 2>&1 || true
  kubectl logs pod/xpushare-e4-b > "${OUT_DIR}/e4-${i}-b.log" 2>&1 || true
  dump_describe xpushare-e4-a "${OUT_DIR}/e4-${i}.a.describe.txt"
  dump_describe xpushare-e4-b "${OUT_DIR}/e4-${i}.b.describe.txt"

  pa="$(phase xpushare-e4-a)"
  pb="$(phase xpushare-e4-b)"
  if [[ "${pa}" != "Succeeded" || "${pb}" != "Succeeded" ]]; then
    invalid_runs+=("${i}:not_succeeded(${pa},${pb})")
    continue
  fi

  if ! has_result_line "${OUT_DIR}/e4-${i}-a.log"; then
    invalid_runs+=("${i}:a_missing_result_line")
    continue
  fi
  if ! has_result_line "${OUT_DIR}/e4-${i}-b.log"; then
    invalid_runs+=("${i}:b_missing_result_line")
    continue
  fi

  echo "[$(ts)] >>> run=${i} parse fairness"
  if python3 "${PY_PARSE}" \
    --log "${OUT_DIR}/e4-${i}-a.log" \
    --log "${OUT_DIR}/e4-${i}-b.log" \
    --out "${OUT_DIR}/e4-${i}-fairness.csv" \
    > "${OUT_DIR}/e4-${i}.parse.txt" 2>&1; then
    valid_csvs+=("${OUT_DIR}/e4-${i}-fairness.csv")
  else
    echo "[$(ts)][WARN] run=${i} parse failed; see ${OUT_DIR}/e4-${i}.parse.txt"
    invalid_runs+=("${i}:parse_failed")
  fi
done

echo "[$(ts)] >>> summarize valid runs: n=${#valid_csvs[@]}"
printf "%s\n" "${invalid_runs[@]}" > "${OUT_DIR}/e4.invalid_runs.txt"

if (( ${#valid_csvs[@]} == 0 )); then
  echo "[$(ts)][WARN] no valid runs to summarize. See ${OUT_DIR}/e4.invalid_runs.txt"
  exit 0
fi

python3 "${PY_SUM}" --skip-invalid --csv "${valid_csvs[@]}" --wa 0.2 --wb 0.4 | tee "${OUT_DIR}/e4.summary.csv"

echo "[$(ts)] E4 batch done. Outputs under ${OUT_DIR}"
