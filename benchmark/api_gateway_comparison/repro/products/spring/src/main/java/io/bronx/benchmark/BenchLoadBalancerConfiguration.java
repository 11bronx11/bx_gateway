package io.bronx.benchmark;

import org.springframework.cloud.loadbalancer.core.ServiceInstanceListSupplier;
import org.springframework.context.ConfigurableApplicationContext;
import org.springframework.context.annotation.Bean;

/**
 * Load-balancer child-context configuration. Keep this as a separate top-level
 * class so the gateway application's main context cannot instantiate it.
 */
public class BenchLoadBalancerConfiguration {
  @Bean
  ServiceInstanceListSupplier serviceInstanceListSupplier(ConfigurableApplicationContext context) {
    return ServiceInstanceListSupplier.builder()
        .withDiscoveryClient()
        .withCaching()
        .withHealthChecks()
        .withWeighted()
        .build(context);
  }
}
