FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends gcc libc6-dev make ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY . .
RUN make

FROM debian:bookworm-slim
RUN useradd --create-home --shell /usr/sbin/nologin appuser
WORKDIR /app
COPY --from=build /app/bin/centsage /app/bin/centsage
COPY --from=build /app/public /app/public
USER appuser
ENV PORT=3000
EXPOSE 3000
CMD ["/app/bin/centsage"]
