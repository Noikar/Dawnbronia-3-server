#!/bin/bash
set -e

# Function to gracefully stop all server processes
cleanup() {
    echo "Shutting down Astonia server..."
    pkill -TERM chatserver 2>/dev/null || true
    pkill -TERM server 2>/dev/null || true
    sleep 2
    pkill -KILL chatserver 2>/dev/null || true
    pkill -KILL server 2>/dev/null || true
    echo "Shutdown complete."
    exit 0
}

# Trap signals for graceful shutdown
trap cleanup SIGTERM SIGINT SIGQUIT

# Export environment variables for the server and tools
export AS3_DBHOST="${AS3_DBHOST:-db}"
export AS3_DBUSER="${AS3_DBUSER:-root}"
export AS3_DBPASS="${AS3_DBPASS:-astonia}"
export AS3_DBNAME="${AS3_DBNAME:-merc}"
export AS3_CHATHOST="${AS3_CHATHOST:-localhost}"

# Wait for MySQL to be ready
wait_for_mysql() {
    echo "Waiting for MySQL at ${AS3_DBHOST}..."
    local max_tries=60
    local count=0
    
    while [ $count -lt $max_tries ]; do
        if mysql -h "${AS3_DBHOST}" \
                 -u "${AS3_DBUSER}" -p"${AS3_DBPASS}" \
                 -e "SELECT 1" >/dev/null 2>&1; then
            echo "MySQL is ready!"
            return 0
        fi
        count=$((count + 1))
        echo "MySQL not ready yet... ($count/$max_tries)"
        sleep 2
    done
    
    echo "ERROR: MySQL did not become ready in time"
    return 1
}

# Initialize database if needed
init_database() {
    echo "Checking database..."
    
    # Check if database exists and has tables
    local tables=$(mysql -h "${AS3_DBHOST}" \
                        -u "${AS3_DBUSER}" -p"${AS3_DBPASS}" \
                        -N -e "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='${AS3_DBNAME}'" 2>/dev/null || echo "0")
    
    if [ "$tables" = "0" ] || [ -z "$tables" ]; then
        echo "Initializing database..."
        # Create database if it doesn't exist
        mysql -h "${AS3_DBHOST}" \
              -u "${AS3_DBUSER}" -p"${AS3_DBPASS}" \
              -e "CREATE DATABASE IF NOT EXISTS ${AS3_DBNAME}"
        # Import schema (create tables first)
        mysql -h "${AS3_DBHOST}" \
              -u "${AS3_DBUSER}" -p"${AS3_DBPASS}" \
              "${AS3_DBNAME}" < create_tables.sql
        # Import initial data
        mysql -h "${AS3_DBHOST}" \
              -u "${AS3_DBUSER}" -p"${AS3_DBPASS}" \
              "${AS3_DBNAME}" < merc.sql
        echo "Database initialized."
    else
        echo "Database already initialized ($tables tables found)."
    fi

    # Apply idempotent migrations on every boot, so databases created by an
    # older build pick up schema added since. See migrations.sql.
    echo "Applying migrations..."
    mysql -h "${AS3_DBHOST}" \
          -u "${AS3_DBUSER}" -p"${AS3_DBPASS}" \
          "${AS3_DBNAME}" < migrations.sql
}

# Start the server processes
start_server() {
    echo "Starting Astonia Community Server (v3)..."

    # Provision the server key file if absent. server.c calls
    # config_file(".serverkey") at startup, and config_file() fatally exit(1)s
    # when the file cannot be opened - so this optional key file must always
    # exist or every area server dies immediately.
    if [ ! -f .serverkey ]; then
        echo "svrkey = ${AS3_SVRKEY:-4241}" > .serverkey
        echo "Created default .serverkey"
    fi

    # Start chatserver first
    echo "Starting chatserver..."
    ./chatserver &
    sleep 1
    
    # Discover every startable area instead of hand-maintaining a list: a zone
    # is startable if zones/<N> exists, N is numeric, and it contains a .map.
    # DEFAULT IS ON - every zone boots unless it is explicitly flagged offline,
    # so we never end up with a silently-dead zone (like pents used to be).
    #
    # A zone stays offline if EITHER:
    #   - a marker file zones/<N>/OFFLINE exists (permanent, version-controlled), OR
    #   - N appears in the OFFLINE_AREAS env var (quick ops toggle, no rebuild).
    #
    # Ports are now deterministic (io.c binds 5555+N), so start order no longer
    # affects which port an area gets - discovery order is purely cosmetic.
    OFFLINE_AREAS="${OFFLINE_AREAS:-}"
    SUPERVISOR_INTERVAL="${SUPERVISOR_INTERVAL:-5}"

    # desired_areas: echo the numeric IDs of every area that SHOULD be online right
    # now - it has a zones/<N>/*.map and is not flagged offline (by a zones/<N>/OFFLINE
    # marker or the OFFLINE_AREAS list). Re-evaluated live, so the supervisor picks up
    # /zone on|off marker changes without a restart.
    desired_areas() {
        local zdir area list=""
        for zdir in zones/*/; do
            area="${zdir#zones/}"; area="${area%/}"
            case "$area" in '' | *[!0-9]*) continue ;; esac
            ls "$zdir"*.map >/dev/null 2>&1 || continue
            [ -e "${zdir}OFFLINE" ] && continue
            case " $OFFLINE_AREAS " in *" $area "*) continue ;; esac
            list="$list $area"
        done
        echo "$list" | tr ' ' '\n' | sort -n | tr '\n' ' '
    }

    # area_listening: succeeds if an area server is bound to its port (5555+N).
    # /proc/net/tcp col 4 == 0A is LISTEN; col 2 is HEXIP:HEXPORT (mawk coerces "0x..").
    area_listening() {
        local port=$((5555 + $1))
        awk -v p="$port" 'NR>1 && $4=="0A" { split($2,a,":"); if ((("0x" a[2])+0) == p) f=1 } END { exit(f?0:1) }' /proc/net/tcp
    }

    # Log which zones start held-offline (boot-time visibility).
    for zdir in zones/*/; do
        area="${zdir#zones/}"; area="${area%/}"
        case "$area" in '' | *[!0-9]*) continue ;; esac
        ls "$zdir"*.map >/dev/null 2>&1 || continue
        if [ -e "${zdir}OFFLINE" ]; then
            echo "Area $area held OFFLINE (marker): $(head -n1 "${zdir}OFFLINE" 2>/dev/null)"
        else
            case " $OFFLINE_AREAS " in *" $area "*) echo "Area $area held OFFLINE (OFFLINE_AREAS)" ;; esac
        fi
    done

    # Initial start of every desired area. Ports are deterministic (5555+N) so start
    # order is cosmetic; the stagger just smooths load. -e reads config from env.
    for area in $(desired_areas); do
        echo "Starting area $area (port $((5555 + area)))..."
        ./server -e -a "$area" &
        sleep 0.5
    done

    echo "All server processes started!"
    echo "Server is ready for connections."

    # Supervisor: keep the desired set running. Respawns crashed areas (a crash used
    # to mean a dead zone until the next container restart) and starts any area whose
    # OFFLINE marker was just removed by /zone on - all without a restart. An area
    # taken offline evacuates its own players and exits; since it is no longer
    # "desired", we simply never restart it.
    echo "Supervisor active (every ${SUPERVISOR_INTERVAL}s)."
    while true; do
        sleep "$SUPERVISOR_INTERVAL"

        if ! pgrep -x chatserver >/dev/null; then
            echo "Supervisor: chatserver died, restarting..."
            ./chatserver &
        fi

        for area in $(desired_areas); do
            if ! area_listening "$area"; then
                echo "Supervisor: starting area $area (port $((5555 + area)))..."
                ./server -e -a "$area" &
            fi
        done
    done
}

# Create an account (for admin use)
create_account() {
    if [ -z "$2" ] || [ -z "$3" ]; then
        echo "Usage: create_account <email> <password>"
        exit 1
    fi
    ./create_account -e "$2" "$3"
}

# Create a character (for admin use)
create_character() {
    if [ -z "$2" ] || [ -z "$3" ] || [ -z "$4" ]; then
        echo "Usage: create_character <account_id> <name> <class>"
        echo "Classes: MWG (Male Warrior God), FMG (Female Mage God), etc."
        exit 1
    fi
    ./create_character -e "$2" "$3" "$4"
}

# Main command handler
case "${1:-start}" in
    start)
        wait_for_mysql
        init_database
        start_server
        ;;
    create_account)
        create_account "$@"
        ;;
    create_character)
        create_character "$@"
        ;;
    init-db)
        wait_for_mysql
        init_database
        echo "Database initialization complete."
        ;;
    bash|sh)
        exec /bin/bash
        ;;
    *)
        echo "Unknown command: $1"
        echo "Available commands: start, create_account, create_character, init-db, bash"
        exit 1
        ;;
esac
