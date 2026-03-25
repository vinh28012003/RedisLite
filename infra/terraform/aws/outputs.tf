output "cluster_name" {
  description = "EKS cluster name"
  value       = aws_eks_cluster.this.name
}

output "cluster_endpoint" {
  description = "EKS API server endpoint"
  value       = aws_eks_cluster.this.endpoint
}

output "ecr_redis_lite_url" {
  description = "ECR repository URL for redis-lite image"
  value       = aws_ecr_repository.redis_lite.repository_url
}

output "ecr_failover_ctl_url" {
  description = "ECR repository URL for failover-ctl image"
  value       = aws_ecr_repository.failover_ctl.repository_url
}

output "kubeconfig_command" {
  description = "Run this to configure kubectl"
  value       = "aws eks update-kubeconfig --name ${aws_eks_cluster.this.name} --region ${var.region}"
}
