if last | grep "still logged in";then
    exit 0
fi

LAST_ACCESS="$(stat -c'%Y' /var/log/wtmp)"
CURRENT_TIME="$(date +%s)"
DIFF="$((CURRENT_TIME-LAST_ACCESS))"
if [ $DIFF -ge 3600 ];then
    sudo shutdown
fi