#!/bin/bash
# List of nodes to compile
NODE_LIST="energy-node hvac-node"
TARGET="nrf52840"
BOARD="dongle"

function login_sensor(){
    local node_name=$1
    local port=$2

    if [ -n "$node_name" ]; then
        echo "Logging into $node_name sensor on $port..."
        gnome-terminal --tab -- bash -c 'cd ./'$node_name'; make TARGET='$TARGET' BOARD='$BOARD' login PORT='$port''
    else
        echo "Invalid sensor name: $node_name"
    fi
}

function login(){
    login_sensor "energy-node" "/dev/ttyACM0"
    login_sensor "hvac-node" "/dev/ttyACM1"
}

function compile_node(){
    local node_name=$1
    local actual_path=$(pwd)

    cd ./$node_name
    make distclean 2>&1 | grep -E "error|warning|TARGET not defined, using target 'native'" | grep -v "CC "
    make TARGET=$TARGET BOARD=$BOARD $node_name 2>&1 | grep -E "error|warning|TARGET not defined, using target 'native'" | grep -v "CC "
    cd "$actual_path"
}

function compile_all_nodes(){
    echo "Start compiling all the nodes:"
    for node in $NODE_LIST
    do
        echo -e "\t - Compiling ${node}..."
        compile_node $node
    done
    echo "All nodes compiled successfully!"
}

function run_cooja(){
    gnome-terminal --tab -- bash -c 'cd ..; cd tools/cooja; ./gradlew run;'
}

function run_rpl_border_router(){
    local target=$1
    if [ "$target" != "cooja" ]; then
        gnome-terminal --tab -- bash -c 'cd rpl-border-router;make TARGET=nrf52840 BOARD=dongle PORT=/dev/ttyACM2 connect-router;'
        echo "Connecting rpl-border-router to dongle"
    else
        gnome-terminal --tab -- bash -c 'cd rpl-border-router;make TARGET=cooja connect-router-cooja;'
        echo "Connecting rpl-border-router to cooja"
    fi
    
}

function run_user_app(){
    gnome-terminal --tab -- bash -c 'cd ./cloud; python3 ./http_server.py;'    
    gnome-terminal --tab -- bash -c 'cd ./cloud; python3 ./user_app.py;'
    echo "HTTP server and User application started successfully!"
}

function run_cloud(){
    local target=$1
    local newdb=$2
    echo "Starting cloud application..."
    gnome-terminal --tab -- bash -c 'cd ./cloud; python3 ./cloud_app.py '$target' '$newdb' --default;'
    echo "Cloud application started successfully!"

    run_user_app
}

# Function to flash a sensor on a specific port
function flash_sensor() {
    local node_name=$1
    local port=$2

    if [ -n "$node_name" ]; then
        echo "Flashing $node_name sensor on $port..."
        cd ./$node_name || exit 1
        make TARGET=$TARGET BOARD=$BOARD ${node_name}.dfu-upload PORT=$port
        cd - > /dev/null || exit 1
    else
        echo "Invalid sensor name: $node_name"
    fi
}

# Function to flash the RPL border router
function flash_rpl_border_router() {
    echo "Flashing RPL border router on /dev/ttyACM2..."
    cd rpl-border-router || exit 1
    make TARGET=$TARGET BOARD=$BOARD PORT=/dev/ttyACM2 border-router.dfu-upload
    cd - > /dev/null || exit 1
}

# Flash function for all nodes
function flash() {
    local ports=("/dev/ttyACM0" "/dev/ttyACM1")
    local index=0

    for node_name in $NODE_LIST; do
        flash_sensor "$node_name" "${ports[$index]}"
        index=$((index + 1))
    done
}

case $1 in
    compile_all)
        compile_all_nodes
        ;;
    compile)
        compile_node $2
        ;;
    cooja)
        run_cooja
        ;;
    border-router)
        run_rpl_border_router $2
        ;;
    cloud_app)
        run_cloud $2 $3
        ;;
    user)
        run_user_app
        ;;
    flash_sensor)
        flash_sensor $2 $3
        ;;
    flash)
        flash
        ;;
    login)
        login
        ;;
    deploy)
        echo "Starting deployment..."
        run_rpl_border_router
        echo "Press any key to start the cloud application..."
        read -n 1 -s
        run_cloud $2 $3
        echo "Press any key to login to the serial output of the dongles..."
        read -n 1 -s
        login
        ;;
    *)
        echo "Command not found"
        exit 1
        ;;
esac