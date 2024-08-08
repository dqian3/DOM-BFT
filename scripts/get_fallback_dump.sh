grep "DUMP.*start.*instance=$1"  ../logs/replica*.log > res.txt
grep "DUMP.*finish.*instance=$1"  ../logs/replica*.log >> res.txt