#!/bin/bash

# DOM-BFT GCP Deployment Script
# This script creates GCP VM instances and generates the configuration files needed for DOM-BFT

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
CONFIG_DIR="$PROJECT_ROOT/configs"
BASE_CONFIG_FILE="$CONFIG_DIR/local.yaml"
OUTPUT_CONFIG_FILE="$CONFIG_DIR/remote-prod.yaml"

# Default deployment configuration
DEFAULT_PROJECT_ID="dom-bft-test"
DEFAULT_ZONES=(
    "us-east1-a" "us-east4-a" "us-west1-a" "us-west4-a"
    "us-east1-b" "us-east4-b" "us-west1-b" "us-west4-b"
    "us-east1-c" "us-east4-c" "us-west1-c" "us-west4-c"
)
DEFAULT_MACHINE_TYPE="t2d-standard-16"
DEFAULT_DISK_SIZE="10GB"
DEFAULT_IMAGE_FAMILY="debian-12"
DEFAULT_IMAGE_PROJECT="debian-cloud"

# DOM-BFT specific defaults
DEFAULT_NUM_REPLICAS=6
DEFAULT_NUM_CLIENTS=8

usage() {
    echo "Usage: $0 <command> [options]"
    echo ""
    echo "Commands:"
    echo "  create           Create GCP instances and generate config"
    echo "  destroy          Delete all instances"
    echo "  status           Show instance status"
    echo "  help             Show this help message"
    echo ""
    echo "Create options:"
    echo "  --project-id ID      GCP project ID (default: $DEFAULT_PROJECT_ID)"
    echo "  --replicas N         Number of replica instances (default: $DEFAULT_NUM_REPLICAS)"
    echo "  --clients N          Number of client instances (default: $DEFAULT_NUM_CLIENTS)"
    echo "  --machine-type TYPE  VM machine type (default: $DEFAULT_MACHINE_TYPE)"
    echo "  --disk-size SIZE     Boot disk size (default: $DEFAULT_DISK_SIZE)"
    echo ""
    echo "Environment variables:"
    echo "  GOOGLE_CLOUD_PROJECT  GCP project ID"
    echo ""
    echo "Examples:"
    echo "  $0 create --project-id my-project --replicas 6 --clients 8"
    echo "  $0 destroy"
    echo "  $0 status"
    echo ""
    echo "Zones used (rotated across all regions and AZs):"
    for zone in "${DEFAULT_ZONES[@]}"; do
        echo "  $zone"
    done
}

# Parse command line arguments
COMMAND=""
PROJECT_ID="${GOOGLE_CLOUD_PROJECT:-$DEFAULT_PROJECT_ID}"
NUM_REPLICAS=$DEFAULT_NUM_REPLICAS
NUM_CLIENTS=$DEFAULT_NUM_CLIENTS
MACHINE_TYPE=$DEFAULT_MACHINE_TYPE
DISK_SIZE=$DEFAULT_DISK_SIZE
ZONES=("${DEFAULT_ZONES[@]}")

while [[ $# -gt 0 ]]; do
    case $1 in
        create|destroy|status|help)
            COMMAND="$1"
            shift
            ;;
        --project-id)
            PROJECT_ID="$2"
            shift 2
            ;;
        --replicas)
            NUM_REPLICAS="$2"
            shift 2
            ;;
        --clients)
            NUM_CLIENTS="$2"
            shift 2
            ;;
        --machine-type)
            MACHINE_TYPE="$2"
            shift 2
            ;;
        --disk-size)
            DISK_SIZE="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

if [[ -z "$COMMAND" ]]; then
    echo "Error: No command specified"
    usage
    exit 1
fi

if [[ "$COMMAND" == "help" ]]; then
    usage
    exit 0
fi

# Check dependencies
check_dependencies() {
    if ! command -v gcloud &> /dev/null; then
        echo "Error: gcloud CLI is not installed"
        echo "Please install the Google Cloud SDK: https://cloud.google.com/sdk/docs/install"
        exit 1
    fi

    if ! command -v yq &> /dev/null; then
        echo "Error: yq is not installed"
        echo "Please install yq: https://github.com/mikefarah/yq"
        exit 1
    fi

    if [[ ! -f "$BASE_CONFIG_FILE" ]]; then
        echo "Error: Base configuration file not found: $BASE_CONFIG_FILE"
        exit 1
    fi
}

# Initialize GCP project
init_gcp() {
    echo "Initializing GCP project: $PROJECT_ID"

    # Set the project
    gcloud config set project "$PROJECT_ID"

    # Enable required APIs
    echo "Enabling required GCP APIs..."
    gcloud services enable compute.googleapis.com

    echo "GCP initialization completed."
}

# Create VPC network and firewall rules
setup_network() {
    echo "Setting up network infrastructure..."

    # Create VPC network
    if ! gcloud compute networks describe dom-bft-network &>/dev/null; then
        echo "Creating VPC network: dom-bft-network"
        gcloud compute networks create dom-bft-network \
            --subnet-mode=auto \
            --bgp-routing-mode=regional
    fi

    # Create firewall rules
    echo "Creating firewall rules..."

    # Allow SSH access
    if ! gcloud compute firewall-rules describe dom-bft-ssh &>/dev/null; then
        gcloud compute firewall-rules create dom-bft-ssh \
            --network=dom-bft-network \
            --allow=tcp:22 \
            --source-ranges=0.0.0.0/0 \
            --description="Allow SSH access to DOM-BFT instances"
    fi

    # Allow DOM-BFT internal communication
    if ! gcloud compute firewall-rules describe dom-bft-internal &>/dev/null; then
        gcloud compute firewall-rules create dom-bft-internal \
            --network=dom-bft-network \
            --allow=tcp:31000-35000 \
            --source-ranges=10.0.0.0/8 \
            --target-tags=dom-bft \
            --description="Allow DOM-BFT internal communication"
    fi

    echo "Network setup completed."
}

# Create VM instances
create_instances() {
    echo "Creating VM instances across all zones..."
    echo "Replicas: $NUM_REPLICAS, Clients: $NUM_CLIENTS"
    echo "Machine type: $MACHINE_TYPE, Disk size: $DISK_SIZE"
    echo "Total zones available: ${#ZONES[@]}"
    echo ""

    # Show zone distribution
    echo "Zone distribution:"
    total_instances=$((NUM_REPLICAS + NUM_CLIENTS))
    for ((i=0; i<total_instances; i++)); do
        zone=${ZONES[$((i % ${#ZONES[@]}))]}
        if [[ $i -lt $NUM_REPLICAS ]]; then
            echo "  replica$i -> $zone"
        else
            client_idx=$((i - NUM_REPLICAS))
            echo "  client$client_idx -> $zone"
        fi
    done
    echo ""

    # Create replica instances in parallel
    echo "Creating replica instances..."
    pids=()
    for ((i=0; i<NUM_REPLICAS; i++)); do
        zone=${ZONES[$((i % ${#ZONES[@]}))]}
        instance_name="replica$i"

        echo "Creating $instance_name in $zone..."
        gcloud compute instances create "$instance_name" \
            --zone="$zone" \
            --machine-type="$MACHINE_TYPE" \
            --network-interface=network-tier=PREMIUM,stack-type=IPV4_ONLY,subnet=dom-bft-network \
            --create-disk=auto-delete=yes,boot=yes,device-name="$instance_name",image=projects/"$DEFAULT_IMAGE_PROJECT"/global/images/family/"$DEFAULT_IMAGE_FAMILY",mode=rw,size="$DISK_SIZE",type=pd-balanced \
            --tags=dom-bft,dom-bft-replica \
            --metadata=startup-script='#!/bin/bash
                apt-get update
                apt-get install -y build-essential cmake git pkg-config
                echo "Instance ready" > /var/log/startup-complete.log' &
        pids+=($!)

        # Slight delay to avoid API rate limits
        sleep 1
    done

    # Create client instances in parallel
    echo "Creating client instances..."
    for ((i=0; i<NUM_CLIENTS; i++)); do
        # Continue the zone rotation from where replicas left off
        zone_index=$(( (NUM_REPLICAS + i) % ${#ZONES[@]} ))
        zone=${ZONES[$zone_index]}
        instance_name="client$i"

        echo "Creating $instance_name in $zone..."
        gcloud compute instances create "$instance_name" \
            --zone="$zone" \
            --machine-type="$MACHINE_TYPE" \
            --network-interface=network-tier=PREMIUM,stack-type=IPV4_ONLY,subnet=dom-bft-network \
            --create-disk=auto-delete=yes,boot=yes,device-name="$instance_name",image=projects/"$DEFAULT_IMAGE_PROJECT"/global/images/family/"$DEFAULT_IMAGE_FAMILY",mode=rw,size="$DISK_SIZE",type=pd-balanced \
            --tags=dom-bft,dom-bft-client \
            --metadata=startup-script='#!/bin/bash
                apt-get update
                apt-get install -y build-essential cmake git pkg-config
                echo "Instance ready" > /var/log/startup-complete.log' &
        pids+=($!)

        # Slight delay to avoid API rate limits
        sleep 1
    done

    # Wait for all instance creations to complete
    echo "Waiting for all instances to be created..."
    for pid in "${pids[@]}"; do
        wait $pid
    done

    echo "All instances created successfully."
}

# Get internal IPs for created instances
get_instance_ips() {
    local instance_type="$1"
    local count="$2"

    local ips=()
    for ((i=0; i<count; i++)); do
        instance_name="${instance_type}$i"
        ip=$(gcloud compute instances describe "$instance_name" \
            --format='get(networkInterfaces[0].networkIP)' \
            --zone=$(gcloud compute instances list --filter="name=$instance_name" --format='get(zone)' | sed 's|.*/||'))
        ips+=("$ip")
    done

    printf '%s\n' "${ips[@]}"
}

# Generate DOM-BFT configuration file based on local.yaml
generate_config() {
    echo "Generating DOM-BFT configuration from $BASE_CONFIG_FILE..."

    # Get instance IPs
    echo "Collecting replica IPs..."
    replica_ips=($(get_instance_ips "replica" "$NUM_REPLICAS"))

    echo "Collecting client IPs..."
    client_ips=($(get_instance_ips "client" "$NUM_CLIENTS"))

    # Copy base config to output
    cp "$BASE_CONFIG_FILE" "$OUTPUT_CONFIG_FILE"

    # Update transport to simple-rpc for remote deployment
    yq -i '.transport = "simple-rpc"' "$OUTPUT_CONFIG_FILE"

    # Update replica IPs
    echo "Setting replica IPs: ${replica_ips[*]}"
    yq -i ".replica.ips = [$(printf '"%s",' "${replica_ips[@]}" | sed 's/,$/')]" "$OUTPUT_CONFIG_FILE"

    # Update client IPs
    echo "Setting client IPs: ${client_ips[*]}"
    yq -i ".client.ips = [$(printf '"%s",' "${client_ips[@]}" | sed 's/,$/')]" "$OUTPUT_CONFIG_FILE"

    # Set proxy IPs to first few client IPs (DOM-BFT uses clients as proxies)
    proxy_count=$((NUM_CLIENTS < 4 ? NUM_CLIENTS : 4))
    proxy_ips=("${client_ips[@]:0:$proxy_count}")
    echo "Setting proxy IPs: ${proxy_ips[*]}"
    yq -i ".proxy.ips = [$(printf '"%s",' "${proxy_ips[@]}" | sed 's/,$/')]" "$OUTPUT_CONFIG_FILE"

    # Adjust timeouts for remote deployment (higher latency)
    yq -i '.client.normalPathTimeout = 1000000' "$OUTPUT_CONFIG_FILE"      # 1s
    yq -i '.client.slowPathTimeout = 100000000' "$OUTPUT_CONFIG_FILE"      # 100s
    yq -i '.client.requestTimeout = 2000000' "$OUTPUT_CONFIG_FILE"         # 2s
    yq -i '.proxy.maxOwd = 200000' "$OUTPUT_CONFIG_FILE"                   # 200ms

    # Increase performance settings for cloud deployment
    yq -i '.client.maxInFlight = 500' "$OUTPUT_CONFIG_FILE"
    yq -i '.client.sendRate = 1000' "$OUTPUT_CONFIG_FILE"
    yq -i '.client.runtimeSeconds = 120' "$OUTPUT_CONFIG_FILE"
    yq -i '.replica.numSendThreads = 6' "$OUTPUT_CONFIG_FILE"
    yq -i '.replica.numVerifyThreads = 6' "$OUTPUT_CONFIG_FILE"

    # Set resilience parameters based on replica count
    # For DOM-BFT: n = 3f + 2e + 1
    if [[ $NUM_REPLICAS -ge 6 ]]; then
        f=1
        e=1
    elif [[ $NUM_REPLICAS -ge 4 ]]; then
        f=1
        e=0
    else
        f=0
        e=1
    fi

    echo "Setting resilience parameters: f=$f, e=$e"
    yq -i ".resiliency.f = $f" "$OUTPUT_CONFIG_FILE"
    yq -i ".resiliency.e = $e" "$OUTPUT_CONFIG_FILE"

    echo "Configuration generated: $OUTPUT_CONFIG_FILE"
    echo ""
    echo "Summary:"
    echo "  Replicas: $NUM_REPLICAS (${replica_ips[*]})"
    echo "  Clients: $NUM_CLIENTS (${client_ips[*]})"
    echo "  Proxies: $proxy_count (${proxy_ips[*]})"
    echo "  Resilience: f=$f, e=$e"
    echo "  Transport: simple-rpc"
    echo ""
}

# Delete all instances
destroy_instances() {
    echo "Destroying all DOM-BFT instances..."

    # Get all instances with dom-bft tag
    instances=$(gcloud compute instances list --filter="tags.items=dom-bft" --format="value(name,zone)" | awk '{print $1 ":" $2}')

    if [[ -z "$instances" ]]; then
        echo "No DOM-BFT instances found."
        return
    fi

    echo "Found instances to delete:"
    echo "$instances" | sed 's/^/  /'
    echo ""

    # Delete instances in parallel
    pids=()
    while IFS=':' read -r name zone; do
        if [[ -n "$name" && -n "$zone" ]]; then
            echo "Deleting $name in $zone..."
            gcloud compute instances delete "$name" --zone="$zone" --quiet &
            pids+=($!)
        fi
    done <<< "$instances"

    # Wait for all deletions to complete
    echo "Waiting for all deletions to complete..."
    for pid in "${pids[@]}"; do
        wait $pid
    done

    echo "All instances deleted successfully."
}

# Show instance status
show_status() {
    echo "DOM-BFT Instance Status:"
    echo "======================"

    instances=$(gcloud compute instances list --filter="tags.items=dom-bft" --format="table(name,status,zone,machineType.basename(),networkInterfaces[0].networkIP:label=INTERNAL_IP,networkInterfaces[0].accessConfigs[0].natIP:label=EXTERNAL_IP)")

    if [[ -z "$instances" ]] || [[ "$instances" == *"Listed 0 items"* ]]; then
        echo "No DOM-BFT instances found."
        echo ""
        echo "To create instances, run:"
        echo "  $0 create --project-id $PROJECT_ID"
        return
    fi

    echo "$instances"
    echo ""

    # Count instances by type
    replica_count=$(gcloud compute instances list --filter="tags.items=dom-bft-replica" --format="value(name)" | wc -l)
    client_count=$(gcloud compute instances list --filter="tags.items=dom-bft-client" --format="value(name)" | wc -l)

    echo "Summary: $replica_count replicas, $client_count clients"

    # Check if config file exists and is up to date
    if [[ -f "$OUTPUT_CONFIG_FILE" ]]; then
        echo "Configuration: $OUTPUT_CONFIG_FILE (exists)"
    else
        echo "Configuration: $OUTPUT_CONFIG_FILE (missing - run create command)"
    fi
}

# Main execution
case "$COMMAND" in
    create)
        check_dependencies
        init_gcp
        setup_network
        create_instances
        echo "Waiting 30 seconds for instances to fully boot..."
        sleep 30
        generate_config
        echo ""
        echo "=== DOM-BFT Deployment Complete ==="
        echo ""
        echo "Next steps:"
        echo "  1. SSH to instances: gcloud compute ssh <instance-name> --zone=<zone>"
        echo "  2. Clone and build DOM-BFT on each instance"
        echo "  3. Use configuration file: $OUTPUT_CONFIG_FILE"
        echo "  4. Run experiments with: invoke gcloud.run --config-file $OUTPUT_CONFIG_FILE"
        echo ""
        show_status
        ;;
    destroy)
        check_dependencies
        destroy_instances
        echo ""
        echo "Cleanup network resources? (firewall rules and VPC will remain for reuse)"
        echo "To manually clean up: gcloud compute networks delete dom-bft-network"
        ;;
    status)
        check_dependencies
        show_status
        ;;
    *)
        echo "Error: Unknown command '$COMMAND'"
        usage
        exit 1
        ;;
esac