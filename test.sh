#set -x

function test()
{
	./colcc gcc main.c -E -o main_1.i
	if [[ $? -eq 0 ]]
	then
		echo -e "\e[32mpass\e[0m"
	else
		echo -e "\e[31mfail\e[0m"
	fi

	./colcc gcc main_1.i -c -o main_1.o
	if [[ $? -eq 0 ]]
	then
		echo -e "\e[32mpass\e[0m"
	else
		echo -e "\e[31mfail\e[0m"
	fi

	./colcc gcc util.c -E -o util_1.i
	if [[ $? -eq 0 ]]
	then
		echo -e "\e[32mpass\e[0m"
	else
		echo -e "\e[31mfail\e[0m"
	fi

	./colcc gcc util_1.i -c -o util_1.o
	if [[ $? -eq 0 ]]
	then
		echo -e "\e[32mpass\e[0m"
	else
		echo -e "\e[31mfail\e[0m"
	fi

	./colcc gcc -O3 -o main_1 main_1.o util_1.o -luuid
	if [[ $? -eq 0 ]]
	then
		echo -e "\e[32mpass\e[0m"
	else
		echo -e "\e[31mfail\e[0m"
	fi

	./main_1 gcc --version
	if [[ $? -eq 0 ]]
	then
		echo -e "\e[32mpass\e[0m"
	else
		echo -e "\e[31mfail\e[0m"
	fi
}

rm -f ./*.o
rm -f ./colcc
make
export DEBUG=1
echo local test...
export LOCAL=1
rm -f ./*.i
rm -f ./*.o
test

echo remote test...
export LOCAL=0
test

rm -f ./*.o
time make
rm -f ./*.o
export DEBUG=0
time make CC="./colcc gcc"
rm -f ./main_*
rm -f ./util_*
rm -f ./*.i
rm -f ./*.o
./colcc --help