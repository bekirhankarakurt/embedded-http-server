-- wrk script: validated configuration update through the JSON API.
wrk.method = "PUT"
wrk.headers["Content-Type"] = "application/json"
wrk.body = '{"hostname":"bench-node","telemetry_interval_ms":500}'
