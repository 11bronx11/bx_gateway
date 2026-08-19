package io.bronx.benchmark;

import com.github.benmanes.caffeine.cache.Caffeine;
import io.github.bucket4j.caffeine.CaffeineProxyManager;
import io.github.bucket4j.distributed.proxy.AsyncProxyManager;
import io.github.bucket4j.distributed.remote.RemoteBucketState;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import javax.crypto.SecretKey;
import javax.crypto.spec.SecretKeySpec;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.boot.ApplicationRunner;
import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.cloud.gateway.filter.GatewayFilterChain;
import org.springframework.cloud.gateway.filter.GlobalFilter;
import org.springframework.cloud.gateway.filter.ratelimit.Bucket4jRateLimiter;
import org.springframework.cloud.gateway.filter.ratelimit.KeyResolver;
import org.springframework.cloud.gateway.route.RouteLocator;
import org.springframework.cloud.gateway.route.builder.GatewayFilterSpec;
import org.springframework.cloud.gateway.route.builder.RouteLocatorBuilder;
import org.springframework.cloud.gateway.support.ipresolver.XForwardedRemoteAddressResolver;
import org.springframework.cloud.loadbalancer.annotation.LoadBalancerClient;
import org.springframework.cloud.loadbalancer.core.ServiceInstanceListSupplier;
import org.springframework.context.ConfigurableApplicationContext;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;
import org.springframework.core.Ordered;
import org.springframework.http.HttpStatus;
import org.springframework.security.config.Customizer;
import org.springframework.security.config.web.server.ServerHttpSecurity;
import org.springframework.security.oauth2.jose.jws.MacAlgorithm;
import org.springframework.security.oauth2.jwt.JwtValidators;
import org.springframework.security.oauth2.jwt.NimbusReactiveJwtDecoder;
import org.springframework.security.oauth2.jwt.ReactiveJwtDecoder;
import org.springframework.security.web.server.SecurityWebFilterChain;
import org.springframework.security.web.server.firewall.ServerExchangeRejectedHandler;
import org.springframework.security.web.server.firewall.StrictServerWebExchangeFirewall;
import org.springframework.stereotype.Component;
import org.springframework.web.server.ServerWebExchange;
import org.springframework.web.reactive.function.server.RouterFunction;
import org.springframework.web.reactive.function.server.RouterFunctions;
import org.springframework.web.reactive.function.server.ServerResponse;
import reactor.core.publisher.Mono;

@SpringBootApplication
@LoadBalancerClient(name = "bench-api", configuration = GatewayApplication.BenchLoadBalancerConfiguration.class)
public class GatewayApplication {
  private static final List<String> RATE_LIMITED_ROUTE_IDS = rateLimitedRouteIds();

  public static void main(String[] args) {
    SpringApplication.run(GatewayApplication.class, args);
  }

  @Bean
  SecurityWebFilterChain security(ServerHttpSecurity http) {
    return http
        .csrf(ServerHttpSecurity.CsrfSpec::disable)
        .httpBasic(ServerHttpSecurity.HttpBasicSpec::disable)
        .formLogin(ServerHttpSecurity.FormLoginSpec::disable)
        .authorizeExchange(exchange -> exchange
            .pathMatchers("/actuator/**").permitAll()
            .anyExchange().hasAuthority("SCOPE_read"))
        .oauth2ResourceServer(resourceServer -> resourceServer.jwt(Customizer.withDefaults()))
        .build();
  }

  @Bean
  ReactiveJwtDecoder jwtDecoder(@Value("${bench.jwt-secret}") String secret,
                                @Value("${bench.jwt-issuer}") String issuer) {
    SecretKey key = new SecretKeySpec(secret.getBytes(StandardCharsets.UTF_8), "HmacSHA256");
    NimbusReactiveJwtDecoder decoder = NimbusReactiveJwtDecoder.withSecretKey(key)
        .macAlgorithm(MacAlgorithm.HS256)
        .build();
    decoder.setJwtValidator(JwtValidators.createDefaultWithIssuer(issuer));
    return decoder;
  }

  @Bean
  StrictServerWebExchangeFirewall requestFirewall() {
    StrictServerWebExchangeFirewall firewall = new StrictServerWebExchangeFirewall();
    firewall.setAllowedParameterValues(value ->
        StrictServerWebExchangeFirewall.ALLOWED_PARAMETER_VALUES.test(value)
            && allowedBenchmarkParameter(value));
    return firewall;
  }

  @Bean
  ServerExchangeRejectedHandler firewallRejectedHandler() {
    return (exchange, rejected) -> {
      exchange.getResponse().setStatusCode(HttpStatus.FORBIDDEN);
      return exchange.getResponse().setComplete();
    };
  }

  @Bean
  @SuppressWarnings({"rawtypes", "unchecked"})
  AsyncProxyManager<String> caffeineProxyManager() {
    Caffeine<String, RemoteBucketState> builder =
        (Caffeine) Caffeine.newBuilder().maximumSize(20_000);
    return new CaffeineProxyManager<>(builder, Duration.ofMinutes(10)).asAsync();
  }

  @Bean
  KeyResolver clientIpKeyResolver() {
    var resolver = XForwardedRemoteAddressResolver.maxTrustedIndex(1);
    return exchange -> Mono.just(resolver.resolve(exchange).getAddress().getHostAddress());
  }

  @Bean
  ApplicationRunner configureBucket4j(
      Bucket4jRateLimiter rateLimiter,
      @Value("${bench.rate-capacity}") long capacity,
      @Value("${bench.rate-refill}") long refill) {
    return ignored -> RATE_LIMITED_ROUTE_IDS.forEach(routeId -> rateLimiter.getConfig().put(
        routeId,
        new Bucket4jRateLimiter.Config()
            .setCapacity(capacity)
            .setRefillTokens(refill)
            .setRefillPeriod(Duration.ofSeconds(1))
            .setRequestedTokens(1)));
  }

  @Bean
  RouteLocator routes(
      RouteLocatorBuilder builder,
      Bucket4jRateLimiter rateLimiter,
      KeyResolver clientIpKeyResolver,
      @Value("${bench.upstream-a}") String upstreamA) {
    var routes = builder.routes();
    routes.route("api", route -> route.path("/api/**")
        .filters(filters -> governed(filters, 1, "v1", rateLimiter, clientIpKeyResolver))
        .uri("lb://bench-api"));
    routes.route("probes", route -> route.path("/probe/rate/**", "/probe/observe/**")
        .filters(filters -> governed(filters, 2, "v1", rateLimiter, clientIpKeyResolver))
        .uri("lb://bench-api"));
    routes.route("ban", route -> route.path("/probe/ban/**")
        .filters(filters -> common(filters.stripPrefix(2), "v1"))
        .uri("lb://bench-api"));
    routes.route("ws", route -> route.path("/ws/**")
        .filters(filters -> common(filters.stripPrefix(1), "v1"))
        .uri("ws://" + upstreamA));
    for (int i = 1; i <= 4; i++) {
      String id = "exact-" + i;
      String path = "/exact/" + i;
      routes.route(id, route -> route.path(path)
          .filters(filters -> governed(filters, 2, "v1", rateLimiter, clientIpKeyResolver))
          .uri("lb://bench-api"));
    }
    for (int i = 1; i <= 10; i++) {
      String id = "route-" + i;
      String path = "/route/" + i + "/**";
      routes.route(id, route -> route.path(path)
          .filters(filters -> governed(filters, 2, "v1", rateLimiter, clientIpKeyResolver))
          .uri("lb://bench-api"));
    }
    return routes.build();
  }

  @Bean
  RouterFunction<ServerResponse> circuitBreakerFallback() {
    return RouterFunctions.route()
        .GET("/__bench/fallback", request ->
            ServerResponse.status(HttpStatus.SERVICE_UNAVAILABLE).build())
        .build();
  }

  private static GatewayFilterSpec governed(
      GatewayFilterSpec filters,
      int stripParts,
      String version,
      Bucket4jRateLimiter rateLimiter,
      KeyResolver keyResolver) {
    return common(filters.stripPrefix(stripParts)
        .circuitBreaker(config -> config.setName("bench-api")
            .setFallbackUri("forward:/__bench/fallback")
            .addStatusCode("500")
            .addStatusCode("502")
            .addStatusCode("503")
            .addStatusCode("504"))
        .requestRateLimiter(config -> config
            .setRateLimiter(rateLimiter)
            .setKeyResolver(keyResolver)
            .setStatusCode(HttpStatus.TOO_MANY_REQUESTS)), version);
  }

  private static GatewayFilterSpec common(GatewayFilterSpec filters, String version) {
    return filters
        .setRequestHeader("X-Bench-Version", version)
        .removeRequestHeader("X-Internal-Debug");
  }

  private static boolean allowedBenchmarkParameter(String value) {
    String normalized = value.toLowerCase(Locale.ROOT);
    return !normalized.contains("' or 1=1")
        && !normalized.contains("<script")
        && !normalized.contains("../")
        && !normalized.contains("sqlmap");
  }

  private static List<String> rateLimitedRouteIds() {
    List<String> ids = new ArrayList<>(List.of(
        "api", "probes", "spring-v2-api", "spring-v2-probes",
        "spring-v2-exact", "spring-v2-route"));
    for (int i = 1; i <= 4; i++) {
      ids.add("exact-" + i);
    }
    for (int i = 1; i <= 10; i++) {
      ids.add("route-" + i);
    }
    return List.copyOf(ids);
  }

  @Configuration(proxyBeanMethods = false)
  static class BenchLoadBalancerConfiguration {
    @Bean
    ServiceInstanceListSupplier serviceInstanceListSupplier(ConfigurableApplicationContext context) {
      return ServiceInstanceListSupplier.builder()
          .withDiscoveryClient()
          .withHealthChecks()
          .withWeighted()
          .build(context);
    }
  }
}

@Component
final class AccessObservationFilter implements GlobalFilter, Ordered {
  private static final Logger LOG = LoggerFactory.getLogger("bench.access");

  @Override
  public int getOrder() {
    return -200;
  }

  @Override
  public Mono<Void> filter(ServerWebExchange exchange, GatewayFilterChain chain) {
    long started = System.nanoTime();
    String supplied = exchange.getRequest().getHeaders().getFirst("X-Request-ID");
    String requestId = supplied == null || supplied.isBlank()
        ? "spring-" + Long.toHexString(started) : supplied;

    // Accessing the firewalled request parameters activates the configured official predicate.
    exchange.getRequest().getQueryParams();
    exchange.getResponse().getHeaders().set("X-Request-ID", requestId);
    var request = exchange.getRequest().mutate().headers(headers ->
        headers.set("X-Request-ID", requestId)).build();
    return chain.filter(exchange.mutate().request(request).build()).doFinally(signal -> {
      int status = exchange.getResponse().getStatusCode() == null
          ? 0 : exchange.getResponse().getStatusCode().value();
      LOG.info("access request_id={} method={} path={} status={} latency_us={}", requestId,
          exchange.getRequest().getMethod(), exchange.getRequest().getURI().getRawPath(), status,
          (System.nanoTime() - started) / 1000);
    });
  }
}
