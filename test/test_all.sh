set -e
set -o pipefail

LOG_FILE="./bisheng_test.log"
ADDR_FILE="./ray_addr.txt"

echo ">>> Stopping any running Ray cluster..."
ray stop --force > "$LOG_FILE" 2>&1 || true

echo ">>> Starting Ray head node..."
ray start --head --resources='{"node0": 1}' >> "$LOG_FILE" 2>&1

#（qeual --address='IP:PORT'）
RAY_ADDR=$(grep -oP "(?<=--address=')\\d+\\.\\d+\\.\\d+\\.\\d+:\\d+" "$LOG_FILE" | tail -n 1 || true)

if [ -z "$RAY_ADDR" ]; then
  echo "Failed to extract RAY address from log:"
  cat "$LOG_FILE"
  exit 1
fi

# export environment
export RAY_ADDRESS=$RAY_ADDR
echo ">>> Using RAY_ADDRESS=$RAY_ADDRESS"


# test component
echo ">>> Running test component..."
set +e
OUTPUT=$(./build/test_component/test_component 2>&1)
STATUS=$?
set -e

echo "$OUTPUT" >> "$LOG_FILE"

if [ $STATUS -ne 0 ]; then
  echo "Test program exited with code $STATUS"
  echo "---- Output ----"
  echo "$OUTPUT"
  echo "----------------"
  ray stop --force >> "$LOG_FILE" 2>&1 || true
  exit 1
fi

if echo "$OUTPUT" | grep -q "test_result_4 = 12"; then
  echo "test_component test pass"
else
  echo "---- test_component output ----"
  echo "$OUTPUT"
  echo "--------------------------------"
  echo "test_component test failed"
fi

# test future
echo ">>> Running test_future..."
set +e
OUTPUT2=$(./build/test_future/test_future 2>&1)
STATUS2=$?
set -e
echo "$OUTPUT2" >> "$LOG_FILE"

if [ $STATUS2 -eq 0 ] && echo "$OUTPUT2" | grep -q "Completed 100/100 tasks"; then
  echo "test_future passed"
else
  echo "---- test_future output ----"
  echo "$OUTPUT2"
  echo "--------------------------------"
  echo "test_future failed"
fi

# test async
echo ">>> Running test_async..."
set +e
OUTPUT3=$(./build/test_async/test_async 2>&1)
STATUS3=$?
set -e
echo "$OUTPUT3" >> "$LOG_FILE"

if [ $STATUS3 -eq 0 ] && echo "$OUTPUT3" | grep -q "196"; then
  echo "test_async passed"
else
  echo "---- test_async output ----"
  echo "$OUTPUT3"
  echo "--------------------------------"
  echo "test_async failed"
fi

# test wait
echo ">>> Running test_wait..."
set +e
OUTPUT4=$(./build/test_wait/test_wait 2>&1)
STATUS4=$?
set -e
echo "$OUTPUT4" >> "$LOG_FILE"

if [ $STATUS4 -eq 0 ] && echo "$OUTPUT4" | grep -q "wait some elapsed"; then
  echo "test_wait passed"
else
  echo "---- test_wait output ----"
  echo "$OUTPUT4"
  echo "--------------------------------"
  echo "test_wait failed"
fi

echo ">>> All Test Done, Stopping Ray..."
ray stop --force >> "$LOG_FILE" 2>&1