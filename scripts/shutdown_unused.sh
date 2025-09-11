if last | grep "still logged in";then
    exit 0
fi

LAST_ACCESS="$(stat -c'%Y' /var/log/wtmp)"
CURRENT_TIME="$(date +%s)"
DIFF="$((CURRENT_TIME-LAST_ACCESS))"

echo $DIFF

if [ $DIFF -ge 600 ];then
    sudo shutdown
fi