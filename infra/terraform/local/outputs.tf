output "cluster_name" {
  description = "Name of the created kind cluster"
  value       = kind_cluster.this.name
}

output "kubeconfig" {
  description = "Kubeconfig content for the cluster"
  value       = kind_cluster.this.kubeconfig
  sensitive   = true
}

output "endpoint" {
  description = "Cluster API server endpoint"
  value       = kind_cluster.this.endpoint
}
