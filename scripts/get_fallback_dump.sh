grep "DUMP.*start.*instance=$1 "  ../logs/replica*.log > $2 
grep "DUMP.*finish.*instance=$1 "  ../logs/replica*.log >> $2