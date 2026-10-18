<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                xmlns:f="http://example.com/f" xmlns:xs="http://www.w3.org/2001/XMLSchema">
   <!-- An initial function writing a result document and a message. -->
   <xsl:include href="inc/included.xsl"/>
   <xsl:param name="s" as="xs:string" static="yes" required="yes"/>
   <xsl:param name="f:p" select="0"/>
   <xsl:function name="f:go" visibility="public">
      <xsl:param name="first" as="xs:integer"/>
      <xsl:param name="second"/>
      <xsl:message select="f:greeting()"/>
      <out s="{$s}" p="{$f:p}" first="{$first}" second="{$second}"/>
   </xsl:function>
</xsl:stylesheet>
