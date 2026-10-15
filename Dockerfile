FROM python:3.12-slim-bookworm
WORKDIR /app
COPY bridge /app/bridge
EXPOSE 8787
CMD ["python", "-m", "bridge", "run", "--config", "/config/config.local.json"]
