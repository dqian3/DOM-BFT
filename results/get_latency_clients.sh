# Should get all client logs
grep "committed.*[0-9]* us" "$@" | grep -o "[0-9]* us" | python3 avg.py