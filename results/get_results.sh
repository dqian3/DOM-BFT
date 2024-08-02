# echo $1


start=$(grep "Client finished initializing" $1 | head -n 1 | grep -o "[0-9]*:[0-9]*:[0-9]*.[0-9]*")
echo $start

end=$(tail -n 1 $1 | grep -o "[0-9]*:[0-9]*:[0-9]*.[0-9]*")
echo $end

timestamp() { 
    date '+%s%N' --date="$1"
}
echo $(( $(( $(timestamp "$end") - $(timestamp "$start") )) / 1000))

cat $1 | grep "committed.*[0-9]* us" | grep -o "[0-9]* us" | python3 avg.py