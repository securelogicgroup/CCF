FROM ubuntu:18.04

RUN echo "APT::Acquire::Retries \"5\";" | tee /etc/apt/apt.conf.d/80-retries

# Work-around for https://github.com/intel/linux-sgx/issues/395
RUN mkdir -p /etc/init

ARG VERSION=0.10.0

RUN mkdir -p /opt/ccf/ccf-$VERSION

COPY . /opt/ccf/ccf-$VERSION

WORKDIR /opt/ccf/ccf-$VERSION/getting_started

RUN apt update \
    && apt install -y ansible software-properties-common bsdmainutils dnsutils \
    && cd setup_vm \
    && ansible-playbook ccf-dependencies-no-driver.yml ci-dependencies.yml \
    && rm -rf /tmp/* \
    && apt remove -y ansible software-properties-common \
    && apt -y autoremove \
    && apt -y clean

RUN mkdir -p /opt/ccf/ccf-$VERSION/build

WORKDIR /opt/ccf/ccf-$VERSION/build

RUN cmake -GNinja .. \
    && ninja