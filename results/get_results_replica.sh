
    

# First entry
start=$(grep "Adding new entry" $1 | head -n 1 | grep -o "[0-9]*:[0-9]*:[0-9]*.[0-9]*")

# Start measuring 10 seconds after start to allow warmup
seconds_start=$(( $(date '+%s' --date="$start") + 5))
start=$(date --date="@$seconds_start" '+%H:%M:%S')


# Cutoff anyhing in file before this line
start_line=$(grep -n "$start" $1 -m 1 | cut -d : -f 1) 

logs=$(tail -n $start_line $1 | grep "Adding new entry")

num_ops=$(echo "$logs" | wc -l)


start_ns=$(date '+%s%N' --date="$(echo "$logs"  | head -n 1 | grep -o "[0-9]*:[0-9]*:[0-9]*.[0-9]*")" )
end_ns=$(date '+%s%N' --date="$(echo "$logs"  | tail -n 1 | grep -o "[0-9]*:[0-9]*:[0-9]*.[0-9]*")" )

echo $num_ops
echo $((($end_ns - $start_ns) / 1000000))