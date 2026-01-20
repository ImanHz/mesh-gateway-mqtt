
#!/usr/bin/bash
mosquitto_sub -h localhost -p 1883 -t /topic/qos1 -v -F "%x" | \
while read -r hex; do
    json=$(bash ./parse_payload.sh "$hex") || continue
    echo $hex
    echo $json
    # mosquitto_pub \
    #     -h scenescape.example.com \
    #     -t /scenescape/sensors/1 \
    #     -m "$json"
done
