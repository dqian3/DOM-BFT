cat <<EOF > temp.yaml
transport: nng 

app: "counter"

client:
  ips:
  - 10.138.0.15
  - 10.182.0.13
  - 10.142.0.14
  - 10.150.0.12
  - 10.138.0.16
  - 10.182.0.14
  - 10.142.0.15
  - 10.150.0.13

  # - 10.138.0.15
  # - 10.182.0.13
  # - 10.142.0.14
  # - 10.150.0.12
  # - 10.138.0.16
  # - 10.182.0.14
  # - 10.142.0.15
  # - 10.150.0.13

  keysDir: keys/client
  port: 33000
  runtimeSeconds: 45
  
  sendMode: sendRate 
  maxInFlight: 500
  sendRate: 200
  requestSize: 512

  normalPathTimeout: 200000   # 200 ms
  slowPathTimeout:   500000   # 1 s
  requestTimeout:   2000000   # 2 s

proxy:
  forwardPort: 31000
  ips:
  - 10.138.0.15
  - 10.182.0.13
  - 10.142.0.14
  - 10.150.0.12
  
  - 10.138.0.16
  - 10.182.0.14
  - 10.142.0.15
  - 10.150.0.13

  keysDir: keys/proxy
  maxOwd: 150000
  measurementPort: 32000
  shards: 1
  offsetCoefficient: 1.5

receiver:
  ips:
  - 10.138.0.14
  - 10.182.0.12
  - 10.142.0.13
  - 10.150.0.11

  keysDir: keys/receiver
  local: true
  numVerifyThreads: 6
  port: 33000
  shards: 1

replica:
  repairTimeout:      1000000
  repairViewTimeout:  5000000
  ips:
  - 10.138.0.14
  - 10.182.0.12
  - 10.142.0.13
  - 10.150.0.11

  keysDir: keys/replica
  numSendThreads: 6
  numVerifyThreads: 6
  port: 34000

  checkpointInterval: 1000
  snapshotInterval:  25000
EOF




invoke gcloud.vm && 
invoke gcloud.run -v 2 --slow-path-freq=10000 --config-file=temp.yaml &&
cat ../logs/replica*.log ../logs/client*.log | grep PERF >slow.out && 

invoke gcloud.run -v 2 --normal-path-freq=10000  --config-file=temp.yaml && 
cat ../logs/replica*.log ../logs/client*.log | grep PERF >normal.out; 


rm temp.yaml
invoke gcloud.vm --stop
