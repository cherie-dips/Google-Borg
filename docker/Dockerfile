# One image for both the Borgmaster and the Borglets (the command picks which).
FROM ubuntu:24.04 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ make \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile ./
COPY third_party third_party
COPY core core
COPY master master
COPY borglet borglet
RUN make -j"$(nproc)" cell

FROM ubuntu:24.04
COPY --from=build /src/build/bin/borgmaster /src/build/bin/borglet /usr/local/bin/
COPY ui /opt/borg/ui
WORKDIR /opt/borg
EXPOSE 8080 9090
CMD ["borgmaster", "--port", "8080", "--ui", "/opt/borg/ui", "--results", "/opt/borg/results"]
