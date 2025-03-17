grep "DUMP.*start.*round=$1 "  ../logs/replica*.log > $2 
grep "DUMP.*finish.*round=$1 "  ../logs/replica*.log >> $2