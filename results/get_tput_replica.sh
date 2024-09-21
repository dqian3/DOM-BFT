
    

# First entry
start=$(grep "seq_=" $1 | head -n 1 | grep -o "[0-9]*:[0-9]*:[0-9]*.[0-9]*")

# Start measuring 5 seconds after start to allow warmup
# seconds_start=$(( $(date '+%s' --date="$start") + 2))
# start=$(date --date="@$seconds_start" '+%H:%M:%S')

# Cutoff anyhing in file before this line
start_line=$(grep -n "$start" $1 -m 1 | cut -d : -f 1) 

logs=$(tail -n "+$start_line" $1 | grep "seq_")

first=$(echo "$logs"  | head -n 1 | awk -F'seq_=' '{print $2}')
last=$(echo "$logs"  | tail -n 1 | awk -F'seq_=' '{print $2}')
num_ops=$(($last - $first))

start_ns=$(date '+%s%N' --date="$(echo "$logs"  | head -n 1 | grep -o "[0-9]*:[0-9]*:[0-9]*.[0-9]*")" )
end_ns=$(date '+%s%N' --date="$(echo "$logs"  | tail -n 1 | grep -o "[0-9]*:[0-9]*:[0-9]*.[0-9]*")" )

secs=$((($end_ns - $start_ns) / 1000000000))
echo "$num_ops operations in $secs sec"
echo $(($num_ops / $secs))